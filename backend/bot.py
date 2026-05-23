#
# Self-hosted Russian streaming voice assistant — Pipecat + SmallWebRTC.
#
# Pipeline:   WebRTC audio in -> Whisper STT (ru) -> Hermes LLM -> Piper TTS (ru) -> WebRTC audio out
# Signaling:  this process serves the browser test client and the /api/offer endpoint over HTTP.
# Media:      WebRTC; the browser reaches this pod through the in-cluster STUNner TURN relay.
#
# Everything (STT, LLM, TTS, VAD) runs locally — no cloud calls in the hot path.
#
import os
import re
from contextlib import asynccontextmanager
from pathlib import Path

import uvicorn
from fastapi import BackgroundTasks, FastAPI
from fastapi.responses import FileResponse, JSONResponse
from loguru import logger

from pipecat.audio.vad.silero import SileroVADAnalyzer
from pipecat.frames.frames import LLMRunFrame
from pipecat.observers.user_bot_latency_observer import UserBotLatencyObserver
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
SYSTEM_PROMPT = (
    os.getenv("SYSTEM_PROMPT", _DEFAULT_SYSTEM_PROMPT) + " " + _NARRATION_INSTRUCTION
)

# Instruction used to make the assistant open the conversation on connect.
GREETING = os.getenv(
    "GREETING",
    "Поприветствуй меня одним коротким предложением и спроси, чем можешь помочь.",
)

APP_DIR = Path(__file__).resolve().parent


# --------------------------------------------------------------------------
# ESP32-compatible SDP munge (forward-compat; gated behind ESP32_COMPAT)
# --------------------------------------------------------------------------
def esp32_munge(sdp: str) -> str:
    """Rewrite an SDP answer for the minimal ESP32-S3 WebRTC stack.

    Pipecat's stock ``--esp32`` munge keeps only ``host`` candidates and drops
    every line containing ``raddr`` — which deletes the TURN ``relay``
    candidate STUNner produces, the only candidate reachable in this topology.
    So we do our own munge instead:

      * drop sha-384/sha-512 fingerprints (ESP32 mbedTLS only does sha-256);
      * keep the STUNner ``relay`` candidate, rewritten as a plain ``host``
        candidate so the ESP32's minimal SDP parser accepts it.

    Browser clients do NOT need this (they handle relay candidates natively);
    it stays OFF until validated against real ESP32-S3 hardware.
    """
    out: list[str] = []
    for line in sdp.splitlines():
        if "sha-384" in line or "sha-512" in line:
            continue
        if line.startswith("a=candidate"):
            if " typ relay" in line:
                line = re.sub(r" raddr \S+ rport \S+", "", line)
                line = line.replace(" typ relay", " typ host")
                out.append(line)
            elif " typ host" in line:
                out.append(line)
            # srflx / prflx candidates are dropped
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
    stt = WhisperSTTService(
        model=WHISPER_MODEL,
        device="cpu",
        compute_type=WHISPER_COMPUTE_TYPE,
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
    user_aggregator, assistant_aggregator = LLMContextAggregatorPair(
        context,
        # Silero VAD drives turn-taking AND barge-in / interruptions.
        user_params=LLMUserAggregatorParams(vad_analyzer=SileroVADAnalyzer()),
    )

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

    task = PipelineTask(
        pipeline,
        params=PipelineParams(
            enable_metrics=True,
            enable_usage_metrics=True,
        ),
        observers=[latency_observer],
    )

    @transport.event_handler("on_client_connected")
    async def on_client_connected(transport, client):
        logger.info("Client connected — sending greeting")
        context.add_message({"role": "user", "content": GREETING})
        await task.queue_frames([LLMRunFrame()])

    @transport.event_handler("on_client_disconnected")
    async def on_client_disconnected(transport, client):
        logger.info("Client disconnected")
        await task.cancel()

    runner = PipelineRunner(handle_sigint=False)
    await runner.run(task)


# --------------------------------------------------------------------------
# HTTP — WebRTC signaling (/api/offer) + the browser test client
# --------------------------------------------------------------------------
_handler = SmallWebRTCRequestHandler()


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
        f"(whisper={WHISPER_MODEL}/{WHISPER_COMPUTE_TYPE}, piper={PIPER_VOICE}, "
        f"esp32_compat={ESP32_COMPAT})"
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


@app.get("/health")
async def health():
    return {"status": "ok"}


@app.get("/")
async def index():
    return FileResponse(APP_DIR / "test-client.html")


if __name__ == "__main__":
    uvicorn.run(app, host=HTTP_HOST, port=HTTP_PORT)
