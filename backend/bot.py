#
# Self-hosted Russian streaming voice assistant — Pipecat + SmallWebRTC.
#
# Pipeline:   WebRTC audio in -> Whisper STT (ru) -> Hermes LLM -> Piper TTS (ru) -> WebRTC audio out
# Signaling:  this process serves the browser test client and the /api/offer endpoint over HTTP.
# Media:      WebRTC; the browser reaches this pod through the in-cluster STUNner TURN relay.
#
# Everything (STT, LLM, TTS, VAD) runs locally — no cloud calls in the hot path.
#
import asyncio
import json
import logging
import os
import re
import time
from contextlib import asynccontextmanager
from pathlib import Path

import uvicorn
from fastapi import BackgroundTasks, FastAPI, Request
from fastapi.responses import FileResponse, JSONResponse, StreamingResponse
from loguru import logger
from pydantic import BaseModel

# aiortc logs DTLS failures via stdlib logging (e.g. "x DTLS handshake failed
# (fingerprint mismatch)" / "x DTLS handshake failed (error <SSL.Error>)" —
# the only place that tells you *why* a peer connection went straight to
# failed without an obvious ICE error). pipecat uses loguru, which doesn't
# forward stdlib logging by default, so those lines stay invisible. Wire up
# a basic stdlib handler and crank aiortc's logger to DEBUG so the failure
# reason ends up in pod logs alongside the loguru output.
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(name)s %(levelname)s %(message)s",
)
logging.getLogger("aiortc").setLevel(logging.DEBUG)
logging.getLogger("aioice").setLevel(logging.DEBUG)

from pipecat.audio.vad.silero import SileroVADAnalyzer
from pipecat.frames.frames import (
    LLMFullResponseEndFrame,
    LLMFullResponseStartFrame,
    LLMRunFrame,
    LLMTextFrame,
    TranscriptionFrame,
)
from pipecat.observers.base_observer import BaseObserver, FramePushed
from pipecat.observers.user_bot_latency_observer import UserBotLatencyObserver
from pipecat.services.stt_service import STTService
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.runner import PipelineRunner
from pipecat.pipeline.task import PipelineParams, PipelineTask
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.aggregators.llm_response_universal import (
    LLMContextAggregatorPair,
    LLMUserAggregatorParams,
)
from pipecat.services.openai.llm import OpenAILLMService
from pipecat.services.piper.tts import PiperTTSService
from pipecat.services.whisper.stt import WhisperSTTService

from whisper_fast import FastWhisperSTTService
from pipecat.transcriptions.language import Language
from pipecat.transports.base_transport import TransportParams
from pipecat.transports.smallwebrtc.request_handler import (
    SmallWebRTCPatchRequest,
    SmallWebRTCRequest,
    SmallWebRTCRequestHandler,
)
from pipecat.transports.smallwebrtc.transport import SmallWebRTCTransport

# --------------------------------------------------------------------------
# Configuration — all via env (see config.example.env / the ConfigMap+Secret)
# --------------------------------------------------------------------------
HERMES_BASE_URL = os.getenv(
    "HERMES_BASE_URL", "http://hermes-api.kubevirt-vms.svc.cluster.local:8642/v1"
)
HERMES_MODEL = os.getenv("HERMES_MODEL", "")
HERMES_API_KEY = os.getenv("HERMES_API_KEY", "")

WHISPER_MODEL = os.getenv("WHISPER_MODEL", "deepdml/faster-whisper-large-v3-turbo-ct2")
WHISPER_COMPUTE_TYPE = os.getenv("WHISPER_COMPUTE_TYPE", "int8")
# faster-whisper defaults are beam_size=5 / best_of=5; we run 1/1 for ~30-50%
# CPU STT speedup at a small accuracy cost. Override per-deploy if needed.
WHISPER_BEAM_SIZE = int(os.getenv("WHISPER_BEAM_SIZE", "1"))
WHISPER_BEST_OF = int(os.getenv("WHISPER_BEST_OF", "1"))

PIPER_VOICE = os.getenv("PIPER_VOICE", "ru_RU-irina-medium")
PIPER_DOWNLOAD_DIR = Path(os.getenv("PIPER_DOWNLOAD_DIR", "/models/piper"))

HTTP_HOST = os.getenv("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.getenv("HTTP_PORT", "7860"))

# ESP32 forward-compat. OFF for the browser milestone. See esp32_munge() below.
ESP32_COMPAT = os.getenv("ESP32_COMPAT", "false").lower() in ("1", "true", "yes")

# STUNner TURN relay — handed to the browser so it can reach this pod.
STUNNER_TURN_URI = os.getenv("STUNNER_TURN_URI", "")
STUNNER_TURN_USERNAME = os.getenv("STUNNER_TURN_USERNAME", "")
STUNNER_TURN_PASSWORD = os.getenv("STUNNER_TURN_PASSWORD", "")

_DEFAULT_SYSTEM_PROMPT = (
    "Ты — голосовой помощник. Всегда отвечай только на русском языке. "
    "Говори кратко и естественно, как в живом разговоре. Не используй "
    "разметку, списки, эмодзи или код — только обычные законченные "
    "предложения, которые человек произнёс бы вслух."
)

# Tool-narration instruction: with Hermes-agent the assistant may run several
# tools per turn, each adding seconds of wall-clock time. Asking the model to
# announce what it's about to do in one short Russian sentence *before* each
# tool call gives the streaming pipeline something to speak immediately, so
# the user hears the bot within ~2s instead of waiting for the whole agent
# loop. Probed against Hermes 2026-05-23 — narration arrives as standard
# `content` deltas ahead of every `hermes.tool.progress` event.
#
# Always-on: the instruction is appended even when SYSTEM_PROMPT is overridden,
# because perceived-latency in voice is too important to leave up to operators
# remembering to include it.
_NARRATION_INSTRUCTION = (
    "Если для ответа нужно вызвать инструмент, сначала произнеси одно "
    "короткое русское предложение о том, что собираешься сделать "
    "(например «Сейчас проверю погоду» или «Подожди, смотрю в календаре»). "
    "Не повторяйся в финальном ответе. Делай минимум вызовов инструментов."
)

# TTS-formatting instruction: Piper synthesises text literally — "26°C" becomes
# "двадцать шесть цэ" or worse. Tell the model to spell out units, symbols and
# common abbreviations as a human would say them aloud. Probed against Hermes
# 2026-05-24 — "погода в Берлине" answer came back as "градуса Цельсия",
# "километров в час", "процента", no raw symbols.
_TTS_FORMATTING_INSTRUCTION = (
    "Текст ответа будет озвучен синтезатором речи, поэтому пиши его так, "
    "как человек произнёс бы его вслух. Все единицы измерения, символы и "
    "сокращения раскрывай словами: «градусов Цельсия» вместо «°C», "
    "«метров в секунду» вместо «м/с», «километров в час» вместо «км/ч», "
    "«процентов» вместо «%», «евро» вместо «€», «долларов» вместо «$», "
    "«то есть» вместо «т.е.», «и так далее» вместо «и т.д.». Время и даты "
    "тоже произноси словами («половина третьего», «двадцать четвёртого мая»). "
    "Не используй специальные символы, формулы или код."
)
SYSTEM_PROMPT = (
    os.getenv("SYSTEM_PROMPT", _DEFAULT_SYSTEM_PROMPT)
    + " "
    + _NARRATION_INSTRUCTION
    + " "
    + _TTS_FORMATTING_INSTRUCTION
)

# Instruction used to make the assistant open the conversation on connect.
GREETING = os.getenv(
    "GREETING",
    "Поприветствуй меня одним коротким предложением и спроси, чем можешь помочь.",
)
# Don't greet again if a client reconnects within this window. ICE/DTLS can
# flap and pipecat tears down + recreates the pipeline on each disconnect;
# without a cooldown the user hears "Привет, Кирилл" every 30s. 10 min
# default — long enough that a fresh visit feels welcomed, short enough
# that a deliberate device reboot after lunch still gets a greeting.
GREETING_COOLDOWN_SECS = float(os.getenv("GREETING_COOLDOWN_SECS", "600"))
# Wall-clock monotonic seconds at which the most recent greeting fired.
# Process-global — fine for the single-device deployment we have today; if
# we ever support multiple concurrent devices, key this by client identity
# (remote IP or a token the device sends in the offer).
_last_greeted_at: float = 0.0

APP_DIR = Path(__file__).resolve().parent


# --------------------------------------------------------------------------
# ESP32-compatible SDP munge (forward-compat; gated behind ESP32_COMPAT)
# --------------------------------------------------------------------------
def esp32_munge(sdp: str) -> str:
    """Rewrite an SDP answer for the ESP32-S3 + esp_peer WebRTC stack.

    Two transforms:

      * drop sha-384/sha-512 fingerprints (ESP32 mbedTLS only does sha-256);
      * relabel every ``a=candidate ... typ host`` line as ``typ relay``,
        leaving the address unchanged.

    Why relabel (instead of keeping both): esp_peer's pair-selection logic
    won't include its own local relay (STUNner TURN) candidate when the
    remote SDP advertises only ``typ host`` candidates — empirically true
    under ICE transport policy ALL too, not just RELAY. Worse, when both
    types are present, esp_peer prefers the host pair, sticks with it
    indefinitely retrying binding requests to an unreachable pod IP, and
    times out without ever giving the relay pair a serious try.

    Relabeling host → relay (and dropping anything else) leaves esp_peer
    with exactly one remote candidate, forcing it down the TURN path:

      device_relay  →  STUNner  →  backend pod (10.244.x.x:port)
      backend pod   →  STUNner  →  device_relay  (reply via TURN unwrap)

    STUNner's UDPRoute/StaticService already permits traffic to the
    backend pod (that's how the browser test client reaches it), so this
    works in any topology where STUNner is reachable from the device —
    flat LAN, NAT, public internet, all the same path.

    Browser clients do NOT need this (aiortc handles relay×host pairs
    natively); the whole function is gated by ESP32_COMPAT=true.
    """
    out: list[str] = []
    for line in sdp.splitlines():
        if "sha-384" in line or "sha-512" in line:
            continue
        if line.startswith("a=candidate"):
            if " typ host" in line:
                out.append(line.replace(" typ host", " typ relay"))
            elif " typ relay" in line:
                # Genuine relay candidate (only present if aiortc has ICE
                # servers configured, which we don't currently do). Pass
                # through with raddr/rport stripped — esp_peer's parser
                # has historically been fussy about trailing tokens.
                out.append(re.sub(r" raddr \S+ rport \S+", "", line))
            # srflx / prflx candidates are dropped — esp_peer's older
            # parser choked on them and we have no use for them anyway.
        else:
            out.append(line)
    return "\r\n".join(out) + "\r\n"


# --------------------------------------------------------------------------
# The bot pipeline — one per WebRTC connection
# --------------------------------------------------------------------------
async def run_bot(webrtc_connection):
    """Build and run the STT -> LLM -> TTS pipeline for one connection."""
    logger.info("Starting voice assistant pipeline")

    transport = SmallWebRTCTransport(
        webrtc_connection=webrtc_connection,
        params=TransportParams(
            audio_in_enabled=True,
            audio_out_enabled=True,
            # Smaller output chunks -> lower first-audio latency.
            audio_out_10ms_chunks=2,
        ),
    )

    # STT — faster-whisper, Russian, INT8 on CPU. Models are cached on the PVC
    # via HF_HOME; the OS page cache keeps repeat loads fast.
    # FastWhisperSTTService wraps the underlying WhisperModel.transcribe to
    # inject beam_size/best_of (pipecat's stock WhisperSTTService doesn't
    # expose them).
    stt = FastWhisperSTTService(
        model=WHISPER_MODEL,
        device="cpu",
        compute_type=WHISPER_COMPUTE_TYPE,
        beam_size=WHISPER_BEAM_SIZE,
        best_of=WHISPER_BEST_OF,
        settings=WhisperSTTService.Settings(language=Language.RU),
    )

    # LLM — Hermes via its OpenAI-compatible API.
    # Streaming is hardcoded on by pipecat (BaseOpenAILLMService._build_chat_completion_params
    # always sets stream=True). Critical here: Hermes' agent loop emits narration
    # text deltas between tool calls, and we want TTS to consume them as they arrive
    # rather than waiting for the whole turn. See test_streaming_enabled().
    llm = OpenAILLMService(
        api_key=HERMES_API_KEY or "none",
        base_url=HERMES_BASE_URL,
        settings=OpenAILLMService.Settings(
            model=HERMES_MODEL,
            system_instruction=SYSTEM_PROMPT,
        ),
    )

    # TTS — Piper, Russian voice, in-process. Voice files cached on the PVC.
    # Pipecat chunks TTS at sentence boundaries automatically.
    tts = PiperTTSService(
        download_dir=PIPER_DOWNLOAD_DIR,
        settings=PiperTTSService.Settings(voice=PIPER_VOICE),
    )

    context = LLMContext()
    # After 5 minutes with no speech, we'll wipe the dialogue history
    # (keeping system + tool messages so the LLM still knows who it is
    # and what it can do). Without this the context grows unbounded as
    # long as the WebRTC peer connection stays up — at 15k+ tokens per
    # turn after a long session, each Hermes call becomes slow and the
    # earlier-turns drift seeds confused responses.
    CONTEXT_IDLE_RESET_SECS = 300.0
    user_aggregator, assistant_aggregator = LLMContextAggregatorPair(
        context,
        # Silero VAD drives turn-taking AND barge-in / interruptions.
        # user_idle_timeout fires on_user_turn_idle after the user has
        # been silent for that long; we use that as the reset trigger.
        user_params=LLMUserAggregatorParams(
            vad_analyzer=SileroVADAnalyzer(),
            user_idle_timeout=CONTEXT_IDLE_RESET_SECS,
        ),
    )

    @user_aggregator.event_handler("on_user_turn_idle")
    async def _on_user_idle(aggregator):
        """Drop dialogue history after a long silence.

        The system prompt is carried on OpenAILLMService.Settings
        (``system_instruction`` above) and re-prepended on every call,
        so the LLM still knows who it is after this reset. The next
        user utterance sees a fresh slate.
        """
        try:
            n_before = len(context.get_messages())
            if n_before == 0:
                return
            context.set_messages([])
            logger.info(
                f"context reset after {CONTEXT_IDLE_RESET_SECS:.0f}s idle: "
                f"dropped {n_before} message(s)"
            )
        except Exception as exc:  # noqa: BLE001
            logger.warning(f"context reset failed: {exc!r}")

    pipeline = Pipeline(
        [
            transport.input(),
            stt,
            user_aggregator,
            llm,
            tts,
            transport.output(),
            assistant_aggregator,
        ]
    )

    # Per-stage latency observer. Emits a per-turn breakdown of
    # user-turn / STT / LLM / TTS TTFB so we can attribute end-to-end latency
    # to a specific stage. Requires enable_metrics=True (set just below).
    latency_observer = UserBotLatencyObserver()

    @latency_observer.event_handler("on_latency_measured")
    async def _on_latency_measured(observer, latency_seconds: float):
        logger.info(f"latency: user->bot {latency_seconds:.3f}s")

    @latency_observer.event_handler("on_latency_breakdown")
    async def _on_latency_breakdown(observer, breakdown):
        for label in breakdown.chronological_events():
            logger.info(f"latency-breakdown: {label}")

    pc_id = webrtc_connection.pc_id
    transcript_observer = TranscriptObserver(pc_id)

    task = PipelineTask(
        pipeline,
        params=PipelineParams(
            enable_metrics=True,
            enable_usage_metrics=True,
        ),
        observers=[latency_observer, transcript_observer],
    )

    # Register this session so /api/text can find it. Done before the
    # pipeline runs so the test client can POST as soon as it's connected.
    _active_sessions[pc_id] = (context, task)

    @transport.event_handler("on_client_connected")
    async def on_client_connected(transport, client):
        global _last_greeted_at
        import time
        now = time.monotonic()
        elapsed = now - _last_greeted_at
        if elapsed < GREETING_COOLDOWN_SECS:
            logger.info(
                f"Client connected (pc_id={pc_id}) — skipping greeting "
                f"({elapsed:.0f}s since last, cooldown {GREETING_COOLDOWN_SECS:.0f}s)"
            )
            return
        _last_greeted_at = now
        logger.info(f"Client connected (pc_id={pc_id}) — sending greeting")
        context.add_message({"role": "user", "content": GREETING})
        await task.queue_frames([LLMRunFrame()])

    @transport.event_handler("on_client_disconnected")
    async def on_client_disconnected(transport, client):
        logger.info(f"Client disconnected (pc_id={pc_id})")
        _active_sessions.pop(pc_id, None)
        await task.cancel()

    runner = PipelineRunner(handle_sigint=False)
    try:
        await runner.run(task)
    finally:
        # Safety net in case the disconnect handler didn't fire (e.g. on
        # abnormal close): make sure the session map doesn't leak entries.
        _active_sessions.pop(pc_id, None)


# --------------------------------------------------------------------------
# HTTP — WebRTC signaling (/api/offer) + the browser test client
# --------------------------------------------------------------------------
_handler = SmallWebRTCRequestHandler()

# pc_id -> (context, task) for /api/text injection. Lets the browser test page
# bypass STT by POSTing text directly; the LLM/TTS path runs as usual and the
# audio comes back over the existing WebRTC connection. Populated by run_bot()
# on connect, cleaned up on disconnect.
_active_sessions: dict[str, tuple[LLMContext, PipelineTask]] = {}


# --------------------------------------------------------------------------
# Live transcript stream — SSE feed of every parsed user input + assistant
# reply across all sessions. Drives the live-log panel on the test client.
# --------------------------------------------------------------------------
# Set of asyncio.Queue subscribers. Each /api/transcripts/stream connection
# gets its own queue; transcript events are fan-out broadcast to all of them.
# A bounded maxsize protects against a slow/dead subscriber backing up the
# pipeline — overflow events are dropped for that subscriber only.
_transcript_subscribers: set[asyncio.Queue] = set()
_TRANSCRIPT_QUEUE_MAXSIZE = 256


def broadcast_transcript(event: dict) -> None:
    """Fan out a transcript event to every connected SSE subscriber.

    Safe to call from any task — uses put_nowait, drops events on slow
    consumers rather than blocking the pipeline.
    """
    event = {"ts": time.time(), **event}
    dead = []
    for q in _transcript_subscribers:
        try:
            q.put_nowait(event)
        except asyncio.QueueFull:
            # Subscriber is too slow — let it fall behind rather than
            # stalling the producer. The browser will see a gap.
            pass
        except Exception:  # noqa: BLE001
            dead.append(q)
    for q in dead:
        _transcript_subscribers.discard(q)


class TranscriptObserver(BaseObserver):
    """Pipeline observer that broadcasts parsed user + assistant text.

    User side: a TranscriptionFrame emerging from any STTService is the
    finalised STT result for one user turn — emit immediately.

    Assistant side: the LLM streams tokens as LLMTextFrames between an
    LLMFullResponseStartFrame and an LLMFullResponseEndFrame. We
    accumulate the chunks and emit a single "assistant" event when the
    response ends, so the live log shows one row per turn.
    """

    def __init__(self, pc_id: str):
        super().__init__()
        self._pc_id = pc_id
        self._assistant_buffer: list[str] = []
        self._in_response = False

    async def on_push_frame(self, data: FramePushed):
        frame = data.frame
        if isinstance(frame, TranscriptionFrame) and isinstance(data.source, STTService):
            text = (frame.text or "").strip()
            if text:
                broadcast_transcript({"pc_id": self._pc_id, "role": "user", "text": text})
        elif isinstance(frame, LLMFullResponseStartFrame):
            self._assistant_buffer = []
            self._in_response = True
        elif isinstance(frame, LLMTextFrame) and self._in_response:
            if frame.text:
                self._assistant_buffer.append(frame.text)
        elif isinstance(frame, LLMFullResponseEndFrame):
            self._in_response = False
            text = "".join(self._assistant_buffer).strip()
            self._assistant_buffer = []
            if text:
                broadcast_transcript({"pc_id": self._pc_id, "role": "assistant", "text": text})


def _prewarm_whisper(app: FastAPI) -> None:
    """Load faster-whisper once at startup and run a tiny warmup transcribe.

    The loaded WhisperModel is held on app.state for the process lifetime, so
    the on-disk weights stay in the OS page cache and CTranslate2's library
    state stays initialized. Per-connection WhisperSTTService instances still
    construct their own WhisperModel, but with page-cache + JIT already warm
    that construction is much faster than cold.
    """
    import numpy as np
    from faster_whisper import WhisperModel

    logger.info(f"warmup: loading whisper '{WHISPER_MODEL}' ({WHISPER_COMPUTE_TYPE})")
    model = WhisperModel(WHISPER_MODEL, device="cpu", compute_type=WHISPER_COMPUTE_TYPE)
    # 1s of silence -> exercises the decode path without producing real text.
    silence = np.zeros(16000, dtype=np.float32)
    segments, _ = model.transcribe(silence, language="ru", beam_size=1)
    for _ in segments:
        pass
    app.state.whisper_model = model
    # Make this model the process-wide shared instance — every subsequent
    # FastWhisperSTTService() construction reuses it instead of allocating
    # a fresh ~1.5 GB CTranslate2 model. Without this the pod is OOMKilled
    # after 2-3 reconnects under a client-side retry loop.
    FastWhisperSTTService.set_shared_model(model)
    logger.info("warmup: whisper ready")


def _prewarm_piper(app: FastAPI) -> None:
    """Load the configured Piper voice once at startup and run a tiny synth."""
    from piper import PiperVoice
    from piper.download_voices import download_voice

    voice_path = PIPER_DOWNLOAD_DIR / f"{PIPER_VOICE}.onnx"
    if not voice_path.exists():
        logger.info(f"warmup: downloading piper voice '{PIPER_VOICE}'")
        PIPER_DOWNLOAD_DIR.mkdir(parents=True, exist_ok=True)
        download_voice(PIPER_VOICE, PIPER_DOWNLOAD_DIR)

    logger.info(f"warmup: loading piper voice '{PIPER_VOICE}'")
    voice = PiperVoice.load(voice_path)
    # Synthesize one short phrase to exercise the ONNX inference path.
    for _ in voice.synthesize("привет"):
        pass
    app.state.piper_voice = voice
    logger.info("warmup: piper ready")


@asynccontextmanager
async def lifespan(app: FastAPI):
    logger.info(
        f"Voice assistant starting on :{HTTP_PORT} "
        f"(whisper={WHISPER_MODEL}/{WHISPER_COMPUTE_TYPE} "
        f"beam={WHISPER_BEAM_SIZE} best_of={WHISPER_BEST_OF}, "
        f"piper={PIPER_VOICE}, esp32_compat={ESP32_COMPAT})"
    )
    if not HERMES_MODEL:
        logger.warning("HERMES_MODEL is empty — set it in the ConfigMap.")
    if not HERMES_API_KEY:
        logger.warning("HERMES_API_KEY is empty — set it via the ExternalSecret.")

    # Warm up the heavy local models so the first WebRTC connection doesn't
    # pay the cold-load cost (Whisper mmap + CTranslate2 JIT, Piper ONNX init).
    # We hold the model handles on app.state for the process lifetime so the
    # OS page cache and library state stay warm across connections.
    try:
        _prewarm_whisper(app)
    except Exception as exc:  # noqa: BLE001
        logger.warning(f"warmup: whisper preload skipped ({exc!r})")
    try:
        _prewarm_piper(app)
    except Exception as exc:  # noqa: BLE001
        logger.warning(f"warmup: piper preload skipped ({exc!r})")

    yield
    await _handler.close()


app = FastAPI(lifespan=lifespan)


@app.post("/api/offer")
async def offer(request: SmallWebRTCRequest, background_tasks: BackgroundTasks):
    """WebRTC signaling: receive the browser's offer, return our answer."""

    async def _on_connection(connection):
        background_tasks.add_task(run_bot, connection)

    answer = await _handler.handle_web_request(
        request=request,
        webrtc_connection_callback=_on_connection,
    )
    if answer and ESP32_COMPAT:
        answer["sdp"] = esp32_munge(answer["sdp"])
        logger.info("Applied ESP32-compatible SDP munge to the answer")
    return answer


@app.patch("/api/offer")
async def ice_candidate(request: SmallWebRTCPatchRequest):
    """WebRTC signaling: receive trickled ICE candidates from the browser."""
    await _handler.handle_patch_request(request)
    return {"status": "success"}


class TextInputRequest(BaseModel):
    """Body for /api/text — testing-only hook that bypasses STT."""

    pc_id: str
    text: str


@app.post("/api/text")
async def text_input(request: TextInputRequest):
    """Inject text into an active session as if it came from STT.

    Useful for testing the LLM/TTS path without speaking into a microphone.
    The audio response still comes back over the existing WebRTC connection.
    """
    session = _active_sessions.get(request.pc_id)
    if session is None:
        return JSONResponse(
            {"error": f"no active session for pc_id={request.pc_id!r}"},
            status_code=404,
        )
    text = request.text.strip()
    if not text:
        return JSONResponse({"error": "text is empty"}, status_code=400)

    context, task = session
    logger.info(f"Injecting text into pc_id={request.pc_id}: {text!r}")
    # Emit a "user" event manually — the LLMTextFrame chain still fires
    # through the observer for the assistant reply, but the user side
    # never passes through STT (which is what TranscriptObserver listens
    # for) so we have to broadcast it ourselves.
    broadcast_transcript({"pc_id": request.pc_id, "role": "user", "text": text, "via": "text"})
    context.add_message({"role": "user", "content": text})
    await task.queue_frames([LLMRunFrame()])
    return {"status": "ok"}


@app.get("/ice-servers")
async def ice_servers():
    """ICE config for the browser — points it at the in-cluster STUNner relay."""
    servers = []
    if STUNNER_TURN_URI and STUNNER_TURN_USERNAME:
        servers.append(
            {
                "urls": STUNNER_TURN_URI,
                "username": STUNNER_TURN_USERNAME,
                "credential": STUNNER_TURN_PASSWORD,
            }
        )
    return JSONResponse({"iceServers": servers})


@app.get("/api/transcripts/stream")
async def transcripts_stream(request: Request):
    """Server-Sent Events feed of every user / assistant transcript.

    Each event is a JSON object: {"ts": float, "pc_id": str, "role":
    "user"|"assistant", "text": str, "via"?: "text"}. The browser test
    client subscribes here and prints them into the live log panel.
    """
    queue: asyncio.Queue = asyncio.Queue(maxsize=_TRANSCRIPT_QUEUE_MAXSIZE)
    _transcript_subscribers.add(queue)
    logger.info(f"transcript subscriber added ({len(_transcript_subscribers)} total)")

    async def gen():
        try:
            # Tiny hello so the browser knows the channel is live even
            # before any conversation happens.
            yield f": connected at {time.time():.3f}\n\n"
            while True:
                if await request.is_disconnected():
                    break
                try:
                    event = await asyncio.wait_for(queue.get(), timeout=15.0)
                    yield f"data: {json.dumps(event, ensure_ascii=False)}\n\n"
                except asyncio.TimeoutError:
                    # Keep-alive heartbeat so proxies don't close the
                    # connection on idle.
                    yield ": ping\n\n"
        finally:
            _transcript_subscribers.discard(queue)
            logger.info(
                f"transcript subscriber removed ({len(_transcript_subscribers)} remaining)"
            )

    return StreamingResponse(gen(), media_type="text/event-stream")


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.get("/")
async def index():
    return FileResponse(APP_DIR / "test-client.html")


if __name__ == "__main__":
    uvicorn.run(app, host=HTTP_HOST, port=HTTP_PORT)
