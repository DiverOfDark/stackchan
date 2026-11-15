"""Speech-to-speech engine: helpers, and a session against a fake Live API."""

import asyncio
import base64
import json
from array import array

from websockets.asyncio.server import serve

import live_engine
from live_engine import Leveler, LiveConfig, WordSplitter, chunk_text, is_loud, run_live_session, split_frames

LOUD = array("h", [5000, -5000] * 160).tobytes()   # 20 ms, already at the output level
QUIET = bytes(640)


def test_split_frames_keeps_the_remainder():
    buf = bytearray(LOUD + QUIET + b"\x01\x02\x03")
    assert split_frames(buf) == [LOUD, QUIET]
    assert bytes(buf) == b"\x01\x02\x03"


def test_is_loud():
    assert is_loud(LOUD)
    assert not is_loud(QUIET)
    assert not is_loud(b"")


def test_word_splitter_joins_words_split_across_deltas():
    w = WordSplitter()
    assert w.feed(" Свет на ку") == ["Свет", "на"]
    assert w.feed("хне включён. ") == ["кухне", "включён."]
    assert w.feed("Гото") == []
    assert w.flush() == ["Гото"]
    assert w.flush() == []


def test_chunk_text_cuts_on_sentence_ends():
    text = "Первое предложение. Второе предложение. Третье."
    parts = chunk_text(text, limit=30)
    assert parts == ["Первое предложение.", "Второе предложение. Третье."]
    assert chunk_text("  ") == []
    assert all(len(p) <= 10 for p in chunk_text("x" * 25, limit=10))


class FakeDevice:
    """Stands in for the FastAPI WebSocket of the robot."""

    def __init__(self):
        self.inbox: asyncio.Queue = asyncio.Queue()
        self.events: list[dict] = []
        self.audio = bytearray()
        self.closed = False

    async def receive(self):
        return await self.inbox.get()

    async def send_text(self, text):
        self.events.append(json.loads(text))

    async def send_bytes(self, data):
        self.audio.extend(data)

    async def close(self):
        self.closed = True

    def kinds(self):
        return [e["t"] for e in self.events]


async def _fake_live(script):
    """A Live API stand-in; `script(ws, seen)` plays the model's side."""
    seen: dict = {"start": None, "audio": 0, "commentary": [], "closed": False}

    async def handler(ws):
        async def reader():
            async for raw in ws:
                m = json.loads(raw)
                if m["type"] == "session.start":
                    seen["start"] = m["session"]
                    await ws.send(json.dumps({"type": "session.started", "session": {}}))
                elif m["type"] == "session.input_audio.append":
                    seen["audio"] += len(base64.b64decode(m["audio"]))
                elif m["type"] == "session.commentary.append":
                    seen["commentary"].append(m)
                elif m["type"] == "session.close":
                    seen["closed"] = True
                    await ws.send(json.dumps({"type": "session.closed", "reason": "close_requested", "usage": {"seconds": 1.0}}))
                    return

        r = asyncio.create_task(reader())
        await script(ws, seen)
        await r

    server = await serve(handler, "127.0.0.1", 0)
    port = server.sockets[0].getsockname()[1]
    return server, f"ws://127.0.0.1:{port}", seen


async def _wait(cond, timeout=5.0):
    for _ in range(int(timeout / 0.02)):
        if cond():
            return
        await asyncio.sleep(0.02)
    raise AssertionError("condition not reached")


async def test_turn_with_delegation(monkeypatch):
    async def script(ws, seen):
        await _wait(lambda: seen["audio"] > 0)
        for delta in (" Включи", " свет"):
            await ws.send(json.dumps({"type": "session.input_transcript.delta", "delta": delta}))
        await ws.send(json.dumps({"type": "session.delegation.created", "delegation": {"id": "d1", "type": "delegation", "target": "client"}}))
        await ws.send(json.dumps({"type": "session.output_transcript.delta", "delta": " Сейчас."}))
        await ws.send(json.dumps({"type": "session.output_audio.delta", "delta": base64.b64encode(LOUD * 5 + QUIET * 5).decode()}))
        await _wait(lambda: seen["commentary"])

    server, url, seen = await _fake_live(script)
    monkeypatch.setattr(live_engine, "LIVE_URL", url)
    monkeypatch.setattr(live_engine, "USER_TURN_GAP_S", 0.1)
    monkeypatch.setattr(live_engine, "HANGOVER_S", 0.1)
    asked: list = []
    transcripts: list = []

    async def delegate(history):
        asked.append(history)
        await asyncio.sleep(0.5)   # outlasts the model's "on it" sentence
        return "Свет включён."

    async def context():
        return "Extra context."

    dev = FakeDevice()
    cfg = LiveConfig(
        api_key="k", instructions="Persona.", delegate=delegate, startup_context=context,
        on_transcript=lambda role, text: transcripts.append((role, text)),
    )
    session = asyncio.create_task(run_live_session(dev, cfg))
    dev.inbox.put_nowait({"type": "websocket.receive", "bytes": LOUD})
    await _wait(lambda: seen["commentary"] and "bot_stopped" in dev.kinds())
    dev.inbox.put_nowait({"type": "websocket.disconnect"})
    await asyncio.wait_for(session, 5)
    server.close()

    assert seen["start"]["model"] == live_engine.LIVE_MODEL
    assert seen["start"]["instructions"] == "Persona. Extra context."
    assert seen["start"]["audio"]["format"] == {"type": "audio/pcm", "rate": 16000}
    assert seen["audio"] == len(LOUD)
    assert asked == [[{"role": "user", "content": "Включи свет"}]]
    assert seen["commentary"][0]["delegation_id"] == "d1"
    assert seen["commentary"][0]["content"] == "Свет включён."
    assert seen["closed"] and dev.closed
    # Only the audible part of the model's output reaches the robot.
    assert LOUD * 5 == bytes(dev.audio[: len(LOUD) * 5])
    assert len(dev.audio) <= len(LOUD) * 10
    kinds = dev.kinds()
    assert kinds.index("user_started") < kinds.index("user_stopped") < kinds.index("bot_stopped")
    assert "bot_started" in kinds and "thinking" in kinds
    assert {"t": "user_text", "text": "Включи свет", "final": True} in dev.events
    assert {"t": "bot_text", "text": "Сейчас."} in dev.events
    assert transcripts == [("user", "Включи свет"), ("assistant", "Сейчас.")]


async def test_false_wake_puts_a_capable_device_to_sleep(monkeypatch):
    async def script(ws, seen):
        await _wait(lambda: seen["closed"])

    server, url, seen = await _fake_live(script)
    monkeypatch.setattr(live_engine, "LIVE_URL", url)
    monkeypatch.setattr(live_engine, "NO_SPEECH_S", 0.3)

    async def delegate(history):
        return ""

    dev = FakeDevice()
    await asyncio.wait_for(
        run_live_session(dev, LiveConfig(api_key="k", instructions="x", delegate=delegate, can_sleep=True)), 5
    )
    server.close()
    assert dev.kinds() == ["sleep"]
    assert seen["closed"] and dev.closed and not dev.audio


async def test_false_wake_leaves_an_older_device_alone(monkeypatch):
    async def script(ws, seen):
        await _wait(lambda: seen["closed"])

    server, url, seen = await _fake_live(script)
    monkeypatch.setattr(live_engine, "LIVE_URL", url)
    monkeypatch.setattr(live_engine, "NO_SPEECH_S", 0.2)

    async def delegate(history):
        return ""

    dev = FakeDevice()
    session = asyncio.create_task(
        run_live_session(dev, LiveConfig(api_key="k", instructions="x", delegate=delegate))
    )
    await asyncio.sleep(0.6)
    assert not session.done() and dev.events == []
    dev.inbox.put_nowait({"type": "websocket.disconnect"})
    await asyncio.wait_for(session, 5)
    server.close()


def test_femto_ws_hands_the_hello_to_the_live_engine(monkeypatch):
    import bot
    from fastapi.testclient import TestClient

    calls: list = []

    async def fake_run_live(websocket, session_id, meta):
        calls.append(meta)
        await websocket.close()

    monkeypatch.setattr(bot, "run_live", fake_run_live)
    monkeypatch.setattr(bot, "OPENAI_API_KEY", "k")
    hello = {"device": "femto-stackchan", "name": "Femto", "caps": ["sleep"]}
    with TestClient(bot.app).websocket_connect("/ws/femto") as ws:
        ws.send_text(json.dumps(hello))
    assert calls == [hello]


def test_voice_setting_takes_a_name_or_a_custom_voice_id():
    assert live_engine.voice_setting("marin") == "marin"
    assert live_engine.voice_setting("voice_abc123") == {"id": "voice_abc123"}


async def test_run_live_puts_persona_language_and_voice_in_the_instructions(monkeypatch):
    import bot

    seen: list = []

    async def fake_session(ws, cfg):
        seen.append(cfg)

    monkeypatch.setattr(bot, "run_live_session", fake_session)
    monkeypatch.setattr(bot, "LIVE_VOICE_PROMPT", "A dry, low male voice.")
    monkeypatch.setattr(bot, "OPENAI_API_KEY", "k")
    await bot.run_live(object(), "s1", {"name": "Femto", "honorific": "guv", "lang": "ru", "caps": ["sleep"]})
    cfg = seen[0]
    assert "Femto" in cfg.instructions and "guv" in cfg.instructions
    assert "Always speak Russian." in cfg.instructions
    assert "Your voice: A dry, low male voice." in cfg.instructions
    assert cfg.instructions.endswith(live_engine.LIVE_RULES)
    assert cfg.can_sleep and cfg.session_id == "s1"


def _tone(amplitude, frames):
    import math

    one = array("h", [int(amplitude * math.sin(2 * math.pi * 200 * i / 16000)) for i in range(320)]).tobytes()
    return [one] * frames


def _rms_dbfs(frame):
    import math

    pcm = array("h", frame)
    return 20 * math.log10((sum(x * x for x in pcm) / len(pcm)) ** 0.5 / 32768)


def test_leveler_raises_a_quiet_voice_to_the_target():
    lv = Leveler(target_dbfs=-17, max_gain_db=24)
    quiet = _tone(600, 150)                       # about -38 dBFS
    assert _rms_dbfs(quiet[0]) < -35
    out = [lv.process(f) for f in quiet]
    assert -18 < _rms_dbfs(out[-1]) < -16
    # It gets there gradually: no jump on the first frame.
    assert _rms_dbfs(out[0]) < -35


def test_leveler_never_attenuates_or_clips():
    lv = Leveler(target_dbfs=-17, max_gain_db=24)
    loud = _tone(20000, 50)
    assert [lv.process(f) for f in loud] == loud
    # A peak in an otherwise quiet voice is held under full scale.
    lv = Leveler(target_dbfs=-17, max_gain_db=24, gain=10.0)
    spike = array("h", [300] * 319 + [9000]).tobytes()
    out = array("h", lv.process(spike))
    assert max(out) <= 30000 and out[0] > 300


def test_leveler_leaves_silence_alone_and_starts_from_a_given_gain():
    lv = Leveler(gain=4.0)
    assert lv.process(QUIET) == QUIET and lv.gain == 4.0
    assert array("h", lv.process(array("h", [100] * 320).tobytes()))[0] == 400
