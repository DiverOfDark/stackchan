"""Femto voice through OpenAI's speech-to-speech model (`gpt-live-1`).

One device connection is bridged to one Live API session. The live model
hears the microphone directly and speaks the replies itself, so there is no
separate speech-to-text, turn detection or speech synthesis. Anything it
cannot answer alone it *delegates*: we run the request through the agent LLM
(Hermes) and hand the result back for it to say.

  device ── PCM 16 kHz ──▶ this bridge ──▶ Live API
  device ◀─ PCM + events ─ this bridge ◀── Live API
                               │
                               └─ delegation ──▶ agent LLM (tools, long tasks)

The device speaks 16 kHz PCM16 in binary frames and JSON events in text frames.
One addition: {"t": "sleep"} tells a device that advertised the "sleep"
capability to end the turn silently, because nobody spoke after the wake
word fired.
"""

from __future__ import annotations

import asyncio
import base64
import json
import os
import time
from array import array
from dataclasses import dataclass, field
from typing import Awaitable, Callable

from loguru import logger
from websockets.asyncio.client import connect as ws_connect
from websockets.exceptions import ConnectionClosed

LIVE_URL = os.getenv("OPENAI_LIVE_URL", "wss://api.openai.com/v1/live/sessions")
LIVE_MODEL = os.getenv("OPENAI_LIVE_MODEL", "gpt-live-1")
# A built-in voice name ("marin", …) or the id of a voice made with
# POST /v1/audio/voices ("voice_…"). Empty = the API's default.
LIVE_VOICE = os.getenv("OPENAI_LIVE_VOICE", "").strip()
# The description a prompt-made voice was created from; OpenAI asks for it to
# be repeated in the session instructions.
LIVE_VOICE_PROMPT = os.getenv("OPENAI_LIVE_VOICE_PROMPT", "").strip()


def voice_setting(voice: str):
    """`audio.output.voice` for a voice name or a custom voice id."""
    return {"id": voice} if voice.startswith("voice_") else voice

SAMPLE_RATE = 16000
FRAME_BYTES = 640            # 20 ms
FRAME_S = 0.02
# Speech goes out this far ahead of real time, so network jitter doesn't
# starve the device's playback buffer.
SEND_AHEAD_S = 0.3
# The Live API streams output continuously, silence included. Only audible
# audio is forwarded, so the device's playback buffer drains between replies.
LOUD_PEAK = 200
# Loudness the model's speech is brought to (RMS of the spoken parts), and
# the most it may be amplified to get there.
TARGET_DBFS = float(os.getenv("LIVE_OUTPUT_DBFS", "-17"))
MAX_GAIN_DB = float(os.getenv("LIVE_OUTPUT_MAX_GAIN_DB", "24"))
# The gain the last session settled on: the voice is the same next time, so
# a new session starts there instead of quiet.
_settled_gain = 1.0
HANGOVER_S = 0.4
# A user turn ends when its transcript has been quiet this long.
USER_TURN_GAP_S = 1.0
# No speech this long after the wake word: a false wake.
NO_SPEECH_S = float(os.getenv("LIVE_NO_SPEECH_SECS", "8"))
THINKING_EVERY_S = 3.0
# How long session start may wait for LiveConfig.startup_context once the
# Live API socket is open.
STARTUP_CONTEXT_WAIT_S = 0.5
DELEGATION_TIMEOUT_S = float(os.getenv("LIVE_DELEGATION_TIMEOUT_SECS", "120"))
# Appends are capped at 500 tokens by the API; stay well under it.
APPEND_MAX_CHARS = 1200

LIVE_RULES = (
    "You are the voice of a small desk robot and hear the room through its "
    "microphone. A session starts when the robot's wake word fires, which also "
    "happens by mistake. Stay silent when what you hear is not a person talking "
    "to you: noise, music, a video, or people talking to each other. If you hear "
    "only your name, stay silent and wait: the request follows in the same "
    "breath, and the robot cannot hear while it speaks. Never reply with a bare "
    "acknowledgement such as \"I'm listening\". Answer small talk and common knowledge yourself. Delegate "
    "everything else: anything that needs tools, the internet, current "
    "information, the home, the owner's accounts or files, or careful reasoning. "
    "When you delegate, say one short sentence that you are on it, then wait for "
    "the result and report it faithfully and briefly. Never invent a result."
)


def split_frames(buf: bytearray) -> list[bytes]:
    """Take whole 20 ms frames off the front of `buf`."""
    n = len(buf) // FRAME_BYTES
    frames = [bytes(buf[i * FRAME_BYTES:(i + 1) * FRAME_BYTES]) for i in range(n)]
    del buf[: n * FRAME_BYTES]
    return frames


class Leveler:
    """Brings the model's speech to one loudness, whatever voice it uses.

    Voices differ a lot: a custom "quiet, cold" voice came out 12 dB below
    the default one and was barely audible on the robot's small speaker. The
    gain follows the average level of the speech heard so far, moves slowly so
    words don't pump, and is cut per frame where a peak would clip. It only
    ever amplifies.
    """

    FULL_SCALE = 32768.0
    PEAK_LIMIT = 30000.0
    MAX_STEP = 1.06          # gain change per 20 ms frame (about 0.5 dB)

    def __init__(self, target_dbfs: float = TARGET_DBFS, max_gain_db: float = MAX_GAIN_DB, gain: float = 1.0):
        self._target = self.FULL_SCALE * 10 ** (target_dbfs / 20)
        self._max_gain = 10 ** (max_gain_db / 20)
        self._power = 0.0    # running mean square of speech frames
        self._frames = 0
        self.gain = min(max(gain, 1.0), self._max_gain)

    def process(self, frame: bytes) -> bytes:
        pcm = array("h")
        pcm.frombytes(frame[: len(frame) // 2 * 2])
        if not pcm:
            return frame
        peak = max(max(pcm), -min(pcm))
        if peak >= LOUD_PEAK:
            power = sum(x * x for x in pcm) / len(pcm)
            # Plain average over the first second, then a slow moving one.
            self._frames = min(self._frames + 1, 250)
            self._power += (power - self._power) / self._frames
            want = min(max(self._target / (self._power ** 0.5 or 1.0), 1.0), self._max_gain)
            self.gain = min(max(want, self.gain / self.MAX_STEP), self.gain * self.MAX_STEP)
        gain = min(self.gain, self.PEAK_LIMIT / peak) if peak else self.gain
        if gain <= 1.0:
            return frame
        return array("h", [int(x * gain) for x in pcm]).tobytes()


def is_loud(frame: bytes) -> bool:
    pcm = array("h")
    pcm.frombytes(frame[: len(frame) // 2 * 2])
    return bool(pcm) and max(max(pcm), -min(pcm)) >= LOUD_PEAK


class WordSplitter:
    """Turns transcript deltas (which may split a word) into whole words."""

    def __init__(self):
        self._tail = ""

    def feed(self, delta: str) -> list[str]:
        text = self._tail + delta
        parts = text.split()
        if text and not text[-1].isspace() and parts:
            self._tail = parts.pop()
        else:
            self._tail = ""
        return parts

    def flush(self) -> list[str]:
        tail, self._tail = self._tail, ""
        return [tail] if tail else []


def chunk_text(text: str, limit: int = APPEND_MAX_CHARS) -> list[str]:
    """Split on sentence ends so each piece fits one context append."""
    text = text.strip()
    out: list[str] = []
    while len(text) > limit:
        cut = max(text.rfind(". ", 0, limit), text.rfind("! ", 0, limit), text.rfind("? ", 0, limit))
        cut = cut + 1 if cut > 0 else limit
        out.append(text[:cut].strip())
        text = text[cut:].strip()
    if text:
        out.append(text)
    return out


@dataclass
class LiveConfig:
    api_key: str
    instructions: str
    # Runs one delegated request: (conversation so far) -> answer text.
    delegate: Callable[[list[dict]], Awaitable[str]]
    # Extra instructions that take a network call to build. Fetched while the
    # Live API connects and dropped if slower than that, so it never delays
    # listening. (Appending context to a running session is not an option: the
    # model reads it aloud.)
    startup_context: Callable[[], Awaitable[str | None]] | None = None
    on_transcript: Callable[[str, str], None] | None = None
    can_sleep: bool = False
    session_id: str = "-"
    history: list[dict] = field(default_factory=list)


class LiveSession:
    def __init__(self, device_ws, cfg: LiveConfig):
        self._dev = device_ws
        self._cfg = cfg
        self._live = None
        self._out: asyncio.Queue[bytes] = asyncio.Queue()
        self._user_text = ""
        self._user_active = False
        self._user_last = 0.0
        self._heard_user = False
        self._bot_words = WordSplitter()
        self._bot_text: list[str] = []
        self._bot_speaking = False
        self._delegations: asyncio.Queue[str] = asyncio.Queue()
        self._user_stopped_at: float | None = None
        self._closing = False

    # ---- device side -----------------------------------------------------

    async def _event(self, msg: dict) -> None:
        try:
            await self._dev.send_text(json.dumps(msg, ensure_ascii=False))
        except Exception as exc:  # noqa: BLE001 — a caption must not end the session
            logger.debug(f"live[{self._cfg.session_id}]: event send failed: {exc!r}")

    async def _live_send(self, msg: dict) -> None:
        await self._live.send(json.dumps(msg, ensure_ascii=False))

    async def _uplink(self) -> None:
        """Microphone audio: device -> Live API."""
        while True:
            msg = await self._dev.receive()
            if msg.get("type") == "websocket.disconnect":
                return
            data = msg.get("bytes")
            if data:
                await self._live_send(
                    {"type": "session.input_audio.append", "audio": base64.b64encode(data).decode()}
                )

    async def _downlink(self) -> None:
        """Speech audio: paced to the device a little ahead of real time."""
        global _settled_gain
        leveler = Leveler(gain=_settled_gain)
        next_send = 0.0
        last_loud = 0.0
        while True:
            try:
                frame = await asyncio.wait_for(self._out.get(), HANGOVER_S)
            except asyncio.TimeoutError:
                frame = None
            now = time.monotonic()
            loud = frame is not None and is_loud(frame)
            if loud:
                last_loud = now
                if not self._bot_speaking:
                    self._bot_speaking = True
                    await self._finish_user_turn()
                    if self._user_stopped_at:
                        logger.info(
                            f"live[{self._cfg.session_id}]: reply starts "
                            f"{now - self._user_stopped_at:.2f}s after the user's last word"
                        )
                        self._user_stopped_at = None
                    await self._event({"t": "bot_started"})
            if self._bot_speaking and now - last_loud > HANGOVER_S and self._out.empty():
                self._bot_speaking = False
                await self._finish_bot_turn()
            if frame is None or not self._bot_speaking:
                continue
            if next_send < now:
                next_send = now
            next_send += FRAME_S
            wait = next_send - now - SEND_AHEAD_S
            if wait > 0:
                await asyncio.sleep(wait)
            await self._dev.send_bytes(leveler.process(frame))
            _settled_gain = leveler.gain

    # ---- turns -----------------------------------------------------------

    async def _on_user_delta(self, delta: str) -> None:
        if not delta.strip() and not self._user_active:
            return
        self._heard_user = True
        self._user_last = time.monotonic()
        if not self._user_active:
            self._user_active = True
            self._user_text = ""
            await self._event({"t": "user_started"})
        self._user_text += delta
        await self._event({"t": "user_text", "text": self._user_text.strip(), "final": False})

    async def _finish_user_turn(self) -> None:
        if not self._user_active:
            return
        self._user_active = False
        text = self._user_text.strip()
        self._user_stopped_at = self._user_last
        await self._event({"t": "user_text", "text": text, "final": True})
        await self._event({"t": "user_stopped"})
        if text:
            self._remember("user", text)

    async def _on_bot_delta(self, delta: str) -> None:
        for word in self._bot_words.feed(delta):
            self._bot_text.append(word)
            await self._event({"t": "bot_text", "text": word})

    async def _finish_bot_turn(self) -> None:
        for word in self._bot_words.flush():
            self._bot_text.append(word)
            await self._event({"t": "bot_text", "text": word})
        text = " ".join(self._bot_text)
        self._bot_text = []
        await self._event({"t": "bot_stopped"})
        if text:
            self._remember("assistant", text)

    def _remember(self, role: str, text: str) -> None:
        self._cfg.history.append({"role": role, "content": text})
        logger.info(f"live[{self._cfg.session_id}]: {role}: {text}")
        if self._cfg.on_transcript:
            self._cfg.on_transcript(role, text)

    async def _watchdog(self) -> None:
        """Ends user turns on a transcript gap, and false wakes on silence."""
        started = time.monotonic()
        while True:
            await asyncio.sleep(0.1)
            now = time.monotonic()
            if self._user_active and now - self._user_last > USER_TURN_GAP_S:
                await self._finish_user_turn()
            if not self._heard_user and now - started > NO_SPEECH_S:
                logger.info(
                    f"live[{self._cfg.session_id}]: no speech {NO_SPEECH_S:.0f}s after wake "
                    f"(false wake){'; putting the device to sleep' if self._cfg.can_sleep else ''}"
                )
                if self._cfg.can_sleep:
                    await self._event({"t": "sleep"})
                    # Let the device hang up by itself; closing first would
                    # look like a dropped link and make it reconnect.
                    await asyncio.sleep(2.0)
                    return
                self._heard_user = True  # an older device just times out by itself

    # ---- delegation ------------------------------------------------------

    async def _delegation_worker(self) -> None:
        """One delegation at a time, in the order the model asked."""
        while True:
            delegation_id = await self._delegations.get()
            # In client mode the event carries no request: it is whatever the
            # user just said. Give a trailing transcript a moment to land.
            for _ in range(15):
                if not self._user_active:
                    break
                await asyncio.sleep(0.1)
            await self._finish_user_turn()
            beat = asyncio.create_task(self._thinking())
            t0 = time.monotonic()
            try:
                answer = await asyncio.wait_for(
                    self._cfg.delegate(list(self._cfg.history)), DELEGATION_TIMEOUT_S
                )
                answer = answer.strip() or "The task finished but returned nothing."
                logger.info(
                    f"live[{self._cfg.session_id}]: delegation done in {time.monotonic() - t0:.1f}s: {answer[:200]}"
                )
            except Exception as exc:  # noqa: BLE001
                logger.warning(f"live[{self._cfg.session_id}]: delegation failed: {exc!r}")
                answer = "The task failed: the assistant backend did not answer. Tell the user it could not be done."
            finally:
                beat.cancel()
            for piece in chunk_text(answer):
                await self._live_send(
                    {"type": "session.commentary.append", "delegation_id": delegation_id, "content": piece}
                )

    async def _thinking(self) -> None:
        last = 0.0
        while True:
            now = time.monotonic()
            if not self._bot_speaking and now - last >= THINKING_EVERY_S:
                last = now
                await self._event({"t": "thinking"})
            await asyncio.sleep(0.1)

    # ---- Live API side ---------------------------------------------------

    async def _live_events(self) -> None:
        pending = bytearray()
        async for raw in self._live:
            m = json.loads(raw)
            t = m.get("type")
            if t == "session.output_audio.delta":
                pending.extend(base64.b64decode(m.get("delta", "")))
                for frame in split_frames(pending):
                    self._out.put_nowait(frame)
            elif t == "session.input_transcript.delta":
                await self._on_user_delta(m.get("delta", ""))
            elif t == "session.output_transcript.delta":
                await self._on_bot_delta(m.get("delta", ""))
            elif t == "session.delegation.created":
                self._delegations.put_nowait(m["delegation"]["id"])
            elif t == "error":
                logger.warning(f"live[{self._cfg.session_id}]: API error: {m.get('error')}")
            elif t == "session.closed":
                usage = m.get("usage") or {}
                logger.info(
                    f"live[{self._cfg.session_id}]: closed ({m.get('reason')}), "
                    f"billed {usage.get('seconds', '?')}s"
                )
                return

    async def run(self) -> None:
        cfg = self._cfg
        session: dict = {
            "model": LIVE_MODEL,
            "instructions": cfg.instructions,
            "audio": {"format": {"type": "audio/pcm", "rate": SAMPLE_RATE}},
            "delegation": {"type": "client"},
        }
        if LIVE_VOICE:
            session["audio"]["output"] = {"voice": voice_setting(LIVE_VOICE)}
        t0 = time.monotonic()
        context = asyncio.create_task(cfg.startup_context()) if cfg.startup_context else None
        async with ws_connect(
            LIVE_URL, additional_headers={"Authorization": f"Bearer {cfg.api_key}"}, max_size=None
        ) as live:
            self._live = live
            if context:
                try:
                    extra = await asyncio.wait_for(context, STARTUP_CONTEXT_WAIT_S)
                    if extra:
                        session["instructions"] += " " + extra
                except Exception as exc:  # noqa: BLE001 — optional context
                    logger.info(f"live[{cfg.session_id}]: starting without extra context ({exc!r})")
            await self._live_send({"type": "session.start", "session": session})
            while True:
                m = json.loads(await asyncio.wait_for(live.recv(), 15))
                if m.get("type") == "session.started":
                    break
                if m.get("type") == "error":
                    raise RuntimeError(f"Live API refused the session: {m.get('error')}")
            logger.info(f"live[{cfg.session_id}]: session up in {time.monotonic() - t0:.2f}s")

            tasks = [
                asyncio.create_task(c)
                for c in (
                    self._uplink(),
                    self._downlink(),
                    self._live_events(),
                    self._watchdog(),
                    self._delegation_worker(),
                )
            ]
            try:
                # The device hanging up, the API closing, or a false wake ends it.
                done, _ = await asyncio.wait(
                    [tasks[0], tasks[2], tasks[3]], return_when=asyncio.FIRST_COMPLETED
                )
                for d in done:
                    if d.exception() and not isinstance(d.exception(), ConnectionClosed):
                        logger.warning(f"live[{cfg.session_id}]: ended on {d.exception()!r}")
            finally:
                if self._bot_text:
                    self._remember("assistant", " ".join(self._bot_text) + " …")
                closed = tasks[2].done()
                for task in tasks[:2] + tasks[3:]:
                    task.cancel()
                if not closed:
                    try:
                        await self._live_send({"type": "session.close"})
                        await asyncio.wait_for(tasks[2], 5)
                    except Exception:  # noqa: BLE001 — best effort; usage is then unconfirmed
                        tasks[2].cancel()
                await asyncio.gather(*tasks, return_exceptions=True)


async def run_live_session(device_ws, cfg: LiveConfig) -> None:
    """Bridge one accepted device WebSocket to a Live API session."""
    try:
        await LiveSession(device_ws, cfg).run()
    except Exception as exc:  # noqa: BLE001
        logger.warning(f"live[{cfg.session_id}]: session failed: {exc!r}")
    finally:
        try:
            await device_ws.close()
        except Exception:  # noqa: BLE001 — already closed by the device
            pass
