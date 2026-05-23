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

WHISPER_MODEL = os.getenv("WHISPER_MODEL", "medium")
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
SYSTEM_PROMPT = os.getenv("SYSTEM_PROMPT", _DEFAULT_SYSTEM_PROMPT)

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

    # LLM — Hermes via its OpenAI-compatible API. Token streaming is on by default.
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

    task = PipelineTask(
        pipeline,
        params=PipelineParams(
            enable_metrics=True,
            enable_usage_metrics=True,
        ),
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
