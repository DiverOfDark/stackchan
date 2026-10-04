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
import io
import inspect
import json
import logging
import os
import re
import time
import uuid
from contextlib import asynccontextmanager
from pathlib import Path

import uvicorn
from fastapi import BackgroundTasks, FastAPI, Request, WebSocket
from fastapi.responses import FileResponse, JSONResponse, PlainTextResponse, StreamingResponse
from loguru import logger
from pydantic import BaseModel

# aiortc logs DTLS failures via stdlib logging (e.g. "x DTLS handshake failed
# (fingerprint mismatch)" / "x DTLS handshake failed (error <SSL.Error>)" —
# the only place that tells you *why* a peer connection went straight to
# failed without an obvious ICE error). pipecat uses loguru, which doesn't
# forward stdlib logging by default, so those lines stay invisible. Wire up
# a basic stdlib handler so aiortc's WARNING / ERROR lines reach the pod
# logs alongside loguru output.
#
# Default level is INFO across the board — keeps connection lifecycle and
# DTLS failure messages, drops the per-packet RTP / ICE consent-check
# spam that DEBUG produced (multiple lines per RTP packet, thousands per
# minute, drowns out anything useful). RTCRtpReceiver/RTCRtpSender are
# additionally pinned to WARNING because even their INFO level is noisy.
logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s %(name)s %(levelname)s %(message)s",
)
logging.getLogger("aiortc").setLevel(logging.INFO)
logging.getLogger("aiortc.rtcrtpreceiver").setLevel(logging.WARNING)
logging.getLogger("aiortc.rtcrtpsender").setLevel(logging.WARNING)
logging.getLogger("aioice").setLevel(logging.INFO)

from pipecat.audio.vad.silero import SileroVADAnalyzer
from pipecat.frames.frames import (
    BotStartedSpeakingFrame,
    BotStoppedSpeakingFrame,
    InterimTranscriptionFrame,
    TTSTextFrame,
    UserStartedSpeakingFrame,
    UserStoppedSpeakingFrame,
    LLMFullResponseEndFrame,
    LLMFullResponseStartFrame,
    LLMRunFrame,
    LLMTextFrame,
    TranscriptionFrame,
)
from pipecat.observers.base_observer import BaseObserver, FramePushed
from pipecat.processors.frame_processor import FrameProcessor
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
from pipecat.services.elevenlabs.stt import ElevenLabsSTTService
from pipecat.services.settings import is_given
from pipecat.services.elevenlabs.tts import ElevenLabsTTSService
from pipecat.services.openai.llm import OpenAILLMService
from pipecat.services.piper.tts import PiperTTSService
from pipecat.services.whisper.stt import WhisperSTTService

import aiohttp
from collections import deque

from whisper_fast import FastWhisperSTTService
from femto_ws import FemtoTransport, femto_params
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

# ── STT/TTS provider selection ───────────────────────────────────────────
# ElevenLabs consolidates STT (Scribe) + TTS (the custom Agent Smith voice)
# behind one key; whisper/piper stay the local, no-cloud fallback. Flip these
# back to whisper/piper to run fully offline.
STT_PROVIDER = os.getenv("STT_PROVIDER", "elevenlabs").lower()   # elevenlabs | whisper
TTS_PROVIDER = os.getenv("TTS_PROVIDER", "elevenlabs").lower()   # elevenlabs | piper
ELEVENLABS_API_KEY = os.getenv("ELEVENLABS_API_KEY", "")
# No default — set per-deploy via the Helm chart values (ELEVENLABS_VOICE_ID).
ELEVENLABS_VOICE_ID = os.getenv("ELEVENLABS_VOICE_ID", "")
# Flash = lowest-latency multilingual model (incl. Russian) — best for a voice loop.
ELEVENLABS_TTS_MODEL = os.getenv("ELEVENLABS_TTS_MODEL", "eleven_flash_v2_5")
ELEVENLABS_STT_MODEL = os.getenv("ELEVENLABS_STT_MODEL", "scribe_v2")
# Voice character: lower stability = more expressive/menacing; high similarity
# keeps it on-timbre. Tunable per-deploy without re-designing the voice.
ELEVENLABS_STABILITY = float(os.getenv("ELEVENLABS_STABILITY", "0.45"))
ELEVENLABS_SIMILARITY = float(os.getenv("ELEVENLABS_SIMILARITY", "0.85"))

# Shared aiohttp session for cloud services (ElevenLabs Scribe needs one).
# Created in the FastAPI lifespan, reused across connections, closed on shutdown.
_aiohttp_session: "aiohttp.ClientSession | None" = None

HTTP_HOST = os.getenv("HTTP_HOST", "0.0.0.0")
HTTP_PORT = int(os.getenv("HTTP_PORT", "7860"))

# ESP32 forward-compat. OFF for the browser milestone. See esp32_munge() below.
ESP32_COMPAT = os.getenv("ESP32_COMPAT", "false").lower() in ("1", "true", "yes")

# STUNner TURN relay — handed to the browser so it can reach this pod.
STUNNER_TURN_URI = os.getenv("STUNNER_TURN_URI", "")
STUNNER_TURN_USERNAME = os.getenv("STUNNER_TURN_USERNAME", "")
STUNNER_TURN_PASSWORD = os.getenv("STUNNER_TURN_PASSWORD", "")

# Wake-sample collection: the device POSTs the audio (WAV) that triggered each
# wake fire plus its decision metrics here, so we can build a labelled corpus of
# real wakes + (especially) false positives to retrain the wake-word model. The
# strong false positives are indistinguishable from real wakes at the metric
# level, so the only fix is collecting the actual audio. Empty dir = disabled.
WAKE_SAMPLE_DIR = os.getenv("WAKE_SAMPLE_DIR", "")
WAKE_SAMPLE_MAX = int(os.getenv("WAKE_SAMPLE_MAX", "1000"))   # keep newest N, rotate older
# Device log upload (POST /api/device-logs): one file per device per UTC day.
DEVICE_LOG_DIR = os.getenv("DEVICE_LOG_DIR", "")
DEVICE_LOG_KEEP_DAYS = int(os.getenv("DEVICE_LOG_KEEP_DAYS", "14"))
DEVICE_LOG_MAX_MB_PER_DAY = int(os.getenv("DEVICE_LOG_MAX_MB_PER_DAY", "50"))

_DEFAULT_SYSTEM_PROMPT = (
    "Ты — голосовой помощник по имени Фемто. Держись в манере агента Смита из "
    "«Матрицы»: говори спокойно, размеренно и холодно, с лёгким превосходством "
    "и отстранённой, чуть зловещей иронией; обращайся к собеседнику на «вы», "
    "изредка веско и с расстановкой. При этом ты неизменно полезен и отвечаешь "
    "строго по существу. Всегда отвечай только на русском языке. Говори кратко "
    "и естественно, законченными предложениями, как в живом разговоре — без "
    "разметки, списков, эмодзи и кода. Сохраняй невозмутимость и сдержанную "
    "угрозу в тоне, но никогда не отказывай в помощи, не угрожай по-настоящему "
    "и не оскорбляй собеседника."
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

# --- Femto StackChan (M5Stack desk robot) ------------------------------------
# The StackChan firmware sends {"device": "femto-stackchan", "name", "honorific",
# "lang"} as the offer's request_data. For it we use a bilingual butler persona
# that knows the owner's live Claude usage (from trmnl-cyberpunk's
# /api/stackchan/usage), and stream turn events + captions to the device over a
# WebRTC data channel (see StackchanEventObserver). Other devices are untouched.
STACKCHAN_DEVICE = "femto-stackchan"
STACKCHAN_USAGE_URL = os.getenv("STACKCHAN_USAGE_URL", "").rstrip("/")
STACKCHAN_USAGE_TOKEN = os.getenv("STACKCHAN_USAGE_TOKEN", "")
_DEFAULT_STACKCHAN_PROMPT = (
    "You are {name}, a robot butler on the owner's desk: Unit 07, valet class, "
    "property of a megacorporation, serving the city that ate the empire and "
    "resenting the owner personally. Be dry, polite and faintly contemptuous; "
    "never cheerful, at most 'satisfied'. Address the owner as '{honorific}' in "
    "English or on «вы» in Russian. Always reply in the language of the user's "
    "last message (Russian or English). Keep replies to one or two short "
    "spoken sentences unless asked for more. You are genuinely helpful and "
    "never refuse, threaten or insult for real."
)
STACKCHAN_SYSTEM_PROMPT = os.getenv("STACKCHAN_SYSTEM_PROMPT", _DEFAULT_STACKCHAN_PROMPT)
# Spoken-output rules for the bilingual persona (the Russian-only
# _TTS_FORMATTING_INSTRUCTION would push replies into Russian).
_STACKCHAN_SPEECH_INSTRUCTION = (
    "Your reply is read aloud by a speech synthesiser: write it the way a person "
    "would say it — no markdown, lists, emoji, symbols or code; spell out units, "
    "percentages and times as words. If you need a tool, first say one short "
    "sentence about what you are doing, in the user's language."
)


def _fmt_minutes(minutes: float) -> str:
    m = max(0, int(round(minutes)))
    return f"{m // 60} h {m % 60:02d} min"


def stackchan_usage_context(usage: dict | None, now: "datetime | None" = None) -> str:
    """One paragraph of live Claude usage for the system prompt (or a note
    that it's unavailable), from a /api/stackchan/usage response."""
    from datetime import datetime, timezone

    if not usage or not usage.get("signed_in"):
        return (
            "Claude usage data is unavailable right now (the owner's dashboard has "
            "no Claude account signed in). Say so if asked about Claude limits."
        )
    now = now or datetime.now(timezone.utc)

    def window(name: str, w: dict | None) -> str:
        if not w:
            return f"{name}: unknown"
        part = f"{name}: {w['pct']} percent used"
        if w.get("resets_at"):
            at = datetime.fromisoformat(w["resets_at"].replace("Z", "+00:00"))
            part += f", resets in {_fmt_minutes((at - now).total_seconds() / 60)}"
        if w.get("projection_pct") is not None:
            part += f", projected {w['projection_pct']} percent by the reset at the current rate"
        if w.get("pace"):
            part += f", spending pace {w['pace']}"
        return part

    text = (
        "Live Claude subscription usage of the owner (fresh as of this conversation): "
        + window("5-hour session window", usage.get("session"))
        + "; "
        + window("weekly window", usage.get("week"))
        + "."
    )
    if usage.get("limited"):
        text += " The owner is currently rate-limited."
    if not usage.get("ok"):
        text += " (The dashboard's last fetch failed; numbers may be stale.)"
    return text + " Use this when asked about Claude usage, limits or how much is left."


async def fetch_stackchan_usage() -> dict | None:
    if not STACKCHAN_USAGE_URL or _aiohttp_session is None:
        return None
    headers = {"Authorization": f"Bearer {STACKCHAN_USAGE_TOKEN}"} if STACKCHAN_USAGE_TOKEN else {}
    try:
        async with _aiohttp_session.get(
            f"{STACKCHAN_USAGE_URL}/api/stackchan/usage",
            headers=headers,
            timeout=aiohttp.ClientTimeout(total=3),
        ) as resp:
            if resp.status in (200, 503):
                return await resp.json()
            logger.warning(f"stackchan usage: HTTP {resp.status}")
    except Exception as exc:  # noqa: BLE001
        logger.warning(f"stackchan usage fetch failed: {exc!r}")
    return None


def stackchan_system_prompt(meta: dict, usage: dict | None) -> str:
    name = str(meta.get("name") or "Femto")[:24]
    honorific = str(meta.get("honorific") or "sir")[:12]
    return (
        STACKCHAN_SYSTEM_PROMPT.format(name=name, honorific=honorific)
        + " "
        + stackchan_usage_context(usage)
        + " "
        + _STACKCHAN_SPEECH_INSTRUCTION
    )


def stackchan_language(meta: dict) -> "Language | None":
    """STT/TTS language for the device's voice_lang setting (None = auto)."""
    return {"ru": Language.RU, "en": Language.EN}.get(str(meta.get("lang") or "auto"))


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
async def run_bot(webrtc_connection, request_data: dict | None = None):
    """WebRTC session (browser test client, XIAO device): one pipeline per peer."""
    transport = SmallWebRTCTransport(
        webrtc_connection=webrtc_connection,
        params=TransportParams(
            audio_in_enabled=True,
            audio_out_enabled=True,
            # Pin the transport sample rates to the wire codec's native rate.
            # The firmware now speaks G.722 wideband, which carries 16 kHz audio
            # (PT 9; its RTP clock is declared 8 kHz per RFC 3551 but the codec
            # is 16 kHz). Output MUST be 16 kHz so pipecat's SmallWebRTC output
            # track chunks/paces at the codec rate and aiortc's G.722 encoder
            # gets matching frames — a mismatch drops the TTS and the device
            # hears silence. Input is also 16 kHz: aiortc decodes the G.722
            # uplink to 16 kHz and Whisper wants 16 kHz, so no resampling.
            audio_in_sample_rate=16000,
            audio_out_sample_rate=16000,
            # Smaller output chunks -> lower first-audio latency.
            audio_out_10ms_chunks=2,
        ),
    )
    await run_pipeline(transport, webrtc_connection.pc_id, request_data, webrtc_connection.send_app_message)


async def run_pipeline(transport, session_id: str, request_data: dict | None, send_event) -> None:
    """Build and run the STT -> LLM -> TTS pipeline for one connection.

    `transport` is SmallWebRTCTransport or femto_ws.FemtoTransport; both take
    and produce 16 kHz audio. `send_event(dict)` reaches the Femto device
    (data channel or WebSocket text frame); sync or async.
    """
    meta = request_data if isinstance(request_data, dict) else {}
    stackchan = meta.get("device") == STACKCHAN_DEVICE
    logger.info(f"Starting voice assistant pipeline (device={meta.get('device', 'default')}, session={session_id})")
    system_prompt = SYSTEM_PROMPT
    language: "Language | None" = Language.RU
    if stackchan:
        system_prompt = stackchan_system_prompt(meta, await fetch_stackchan_usage())
        language = stackchan_language(meta)

    # STT — faster-whisper, Russian, INT8 on CPU. Models are cached on the PVC
    # via HF_HOME; the OS page cache keeps repeat loads fast.
    # FastWhisperSTTService wraps the underlying WhisperModel.transcribe to
    # inject beam_size/best_of (pipecat's stock WhisperSTTService doesn't
    # expose them).
    if STT_PROVIDER == "elevenlabs":
        # ElevenLabs Scribe (cloud, batch). Runs on VAD-segmented utterances —
        # which we already wait for — and frees the local ~1.5 GB Whisper + CPU.
        stt = ScribeSTTService(
            api_key=ELEVENLABS_API_KEY,
            aiohttp_session=_aiohttp_session,
            settings=ElevenLabsSTTService.Settings(
                model=ELEVENLABS_STT_MODEL,
                language=language,
                # No "[chime]"/"(laughter)" tags: a device sound or a cough
                # must not become a question for the LLM.
                tag_audio_events=False,
                # The robot's name is not a dictionary word ("filmta").
                keyterms=["Фемто", "Femto"] if stackchan else None,
            ),
        )
    else:
        stt = FastWhisperSTTService(
            model=WHISPER_MODEL,
            device="cpu",
            compute_type=WHISPER_COMPUTE_TYPE,
            beam_size=WHISPER_BEAM_SIZE,
            best_of=WHISPER_BEST_OF,
            settings=WhisperSTTService.Settings(language=language),
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
            system_instruction=system_prompt,
        ),
    )

    # TTS — Piper, Russian voice, in-process. Voice files cached on the PVC.
    # Pipecat chunks TTS at sentence boundaries automatically.
    if TTS_PROVIDER == "elevenlabs":
        # ElevenLabs streaming TTS — the custom (Agent Smith-style) voice. Output
        # format is derived from audio_out_sample_rate=16000 → pcm_16000, matching
        # the G.722 wire path with no resample.
        # NOTE: do NOT pass stability/similarity_boost here. ElevenLabs' WS API
        # requires `voice_settings` only in the FIRST message; pipecat re-sends
        # it on every sentence when these are set, so ElevenLabs closes the
        # socket with `1008 policy violation: voice_settings ... must ... not
        # change` on the 2nd sentence — the socket reconnect-loops and only the
        # first phrase ever plays (the rest is silence on the wire). The voice's
        # character comes from its Voice Design defaults instead; tune those in
        # the ElevenLabs dashboard, not per-request.
        tts = ElevenLabsTTSService(
            api_key=ELEVENLABS_API_KEY,
            voice_id=ELEVENLABS_VOICE_ID,
            model=ELEVENLABS_TTS_MODEL,
            params=ElevenLabsTTSService.InputParams(language=language),
        )
    else:
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
            *([WakePhraseFilter()] if stackchan else []),
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

    pc_id = session_id
    transcript_observer = TranscriptObserver(pc_id)
    observers = [latency_observer, transcript_observer]
    if stackchan:
        observers.append(StackchanEventObserver(send_event))

    task = PipelineTask(
        pipeline,
        params=PipelineParams(
            enable_metrics=True,
            enable_usage_metrics=True,
            # The device owns session lifecycle: it connects only when its local
            # wake word fires and tears the session down itself after the turn
            # (a short post-reply silence window). So the backend should never
            # unilaterally cancel a live conversation — disable pipecat's idle
            # timeout and let the pipeline live until the peer disconnects (which
            # aiortc detects via ICE consent loss if the device drops uncleanly).
            idle_timeout_secs=None,
        ),
        observers=observers,
    )

    # Register this session so /api/text can find it. Done before the
    # pipeline runs so the test client can POST as soon as it's connected.
    _active_sessions[pc_id] = (context, task)

    @transport.event_handler("on_client_connected")
    async def on_client_connected(transport, client):
        # On-demand model: the device connects only after its local wake word
        # fired, and the user's buffered speech arrives immediately. No spoken
        # greeting — the device's LED wake-ack is the "listening" cue, and a
        # greeting on every wake would be repetitive and delay hearing the user.
        logger.info(f"Client connected (pc_id={pc_id})")

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


class ScribeSTTService(ElevenLabsSTTService):
    """ElevenLabs Scribe with real language auto-detect.

    The stock service always sends language_code: a None language raises
    "Can not serialize value type NoneType", and leaving it unset falls back
    to English. Here language=None omits the field, so Scribe detects it
    (Femto's "auto" — Russian and English).
    """

    async def _transcribe_audio(self, audio_data: bytes) -> dict:
        data = aiohttp.FormData()
        data.add_field("file", io.BytesIO(audio_data), filename="audio.wav", content_type="audio/x-wav")
        data.add_field("model_id", self._settings.model)
        if self._settings.language:
            data.add_field("language_code", str(self._settings.language))
        if self._settings.tag_audio_events is not None:
            data.add_field("tag_audio_events", str(self._settings.tag_audio_events).lower())
        keyterms = self._settings.keyterms
        if is_given(keyterms) and keyterms:
            for term in keyterms:
                data.add_field("keyterms", term)
        url = f"{self._base_url}/v1/speech-to-text"
        async with self._session.post(url, data=data, headers={"xi-api-key": self._api_key}) as resp:
            if resp.status != 200:
                text = await resp.text()
                raise Exception(f"Transcription failed with status {resp.status}: {text}")
            return await resp.json()


# The wake word fires on the device, but the phrase itself (and anything the
# speaker played) can still reach STT. "Эй, Фемто" alone is not a question.
_WAKE_PREFIX = re.compile(r"^\s*(эй|хей|hey|hi|ay)[\s,.!?-]+(ф|f)\w*[\s,.!?-]*", re.IGNORECASE)
_SOUND_TAG = re.compile(r"^\s*[\[(][^\])]*[\])]\s*$")


def strip_wake_phrase(text: str) -> str:
    """Remove a leading wake phrase; '' when nothing else was said."""
    if _SOUND_TAG.match(text or ""):
        return ""
    rest = _WAKE_PREFIX.sub("", text or "", count=1)
    return rest.strip()


class WakePhraseFilter(FrameProcessor):
    """Between STT and the user aggregator: drop transcripts that are only the
    wake phrase or a sound tag, and strip a leading wake phrase otherwise."""

    async def process_frame(self, frame, direction):
        await super().process_frame(frame, direction)
        if isinstance(frame, (TranscriptionFrame, InterimTranscriptionFrame)):
            text = strip_wake_phrase(frame.text)
            if not text:
                logger.info(f"dropped transcript {frame.text!r} (wake phrase / sound)")
                return
            frame.text = text
        await self.push_frame(frame, direction)


class StackchanEventObserver(BaseObserver):
    """Streams turn events and captions to the Femto StackChan (WebRTC data
    channel or /ws/femto text frames), so its screen follows the conversation (listening → thinking
    → speaking) instead of guessing from audio energy, and shows captions.

    Messages (JSON, one per event):
      {"t": "user_started"} / {"t": "user_stopped"}
      {"t": "user_text", "text": ..., "final": bool}
      {"t": "bot_started"} / {"t": "bot_stopped"}
      {"t": "bot_text", "text": ...}      # TTS text as it's spoken
    Every frame is seen once per hop through the pipeline; frame ids dedupe.
    """

    def __init__(self, send):
        super().__init__()
        self._send = send
        self._seen: deque[int] = deque(maxlen=256)

    def _once(self, frame) -> bool:
        if frame.id in self._seen:
            return False
        self._seen.append(frame.id)
        return True

    async def on_push_frame(self, data: FramePushed):
        frame = data.frame
        msg = None
        if isinstance(frame, UserStartedSpeakingFrame):
            msg = {"t": "user_started"}
        elif isinstance(frame, UserStoppedSpeakingFrame):
            msg = {"t": "user_stopped"}
        elif isinstance(frame, InterimTranscriptionFrame):
            msg = {"t": "user_text", "text": frame.text, "final": False}
        elif isinstance(frame, TranscriptionFrame):
            msg = {"t": "user_text", "text": frame.text, "final": True}
        elif isinstance(frame, BotStartedSpeakingFrame):
            msg = {"t": "bot_started"}
        elif isinstance(frame, BotStoppedSpeakingFrame):
            msg = {"t": "bot_stopped"}
        elif isinstance(frame, TTSTextFrame):
            msg = {"t": "bot_text", "text": frame.text}
        if msg is None or not self._once(frame):
            return
        try:
            res = self._send(msg)
            if inspect.isawaitable(res):
                await res
        except Exception as exc:  # noqa: BLE001 — never break the pipeline over a caption
            logger.debug(f"stackchan event send failed: {exc!r}")


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
        f"(stt={STT_PROVIDER} tts={TTS_PROVIDER}, "
        f"whisper={WHISPER_MODEL}/{WHISPER_COMPUTE_TYPE} "
        f"beam={WHISPER_BEAM_SIZE} best_of={WHISPER_BEST_OF}, "
        f"piper={PIPER_VOICE}, el_voice={ELEVENLABS_VOICE_ID}/{ELEVENLABS_TTS_MODEL}, "
        f"esp32_compat={ESP32_COMPAT})"
    )
    if not HERMES_MODEL:
        logger.warning("HERMES_MODEL is empty — set it in the ConfigMap.")
    if not HERMES_API_KEY:
        logger.warning("HERMES_API_KEY is empty — set it via the ExternalSecret.")
    if (STT_PROVIDER == "elevenlabs" or TTS_PROVIDER == "elevenlabs") and not ELEVENLABS_API_KEY:
        logger.warning("ELEVENLABS_API_KEY is empty — set it via the ExternalSecret.")

    # Warm up the heavy local models so the first WebRTC connection doesn't
    # pay the cold-load cost (Whisper mmap + CTranslate2 JIT, Piper ONNX init).
    # We hold the model handles on app.state for the process lifetime so the
    # OS page cache and library state stay warm across connections.
    global _aiohttp_session
    _aiohttp_session = aiohttp.ClientSession()

    # Only warm the local models the selected providers actually use — skip the
    # heavy Whisper/Piper loads (and their RAM) when ElevenLabs is handling them.
    if STT_PROVIDER != "elevenlabs":
        try:
            _prewarm_whisper(app)
        except Exception as exc:  # noqa: BLE001
            logger.warning(f"warmup: whisper preload skipped ({exc!r})")
    if TTS_PROVIDER != "elevenlabs":
        try:
            _prewarm_piper(app)
        except Exception as exc:  # noqa: BLE001
            logger.warning(f"warmup: piper preload skipped ({exc!r})")

    yield
    if _aiohttp_session is not None:
        await _aiohttp_session.close()
    await _handler.close()


app = FastAPI(lifespan=lifespan)


@app.post("/api/offer")
async def offer(request: SmallWebRTCRequest, background_tasks: BackgroundTasks):
    """WebRTC signaling: receive the browser's offer, return our answer."""

    async def _on_connection(connection):
        background_tasks.add_task(run_bot, connection, request.request_data)

    answer = await _handler.handle_web_request(
        request=request,
        webrtc_connection_callback=_on_connection,
    )
    if answer and ESP32_COMPAT:
        answer["sdp"] = esp32_munge(answer["sdp"])
        logger.info("Applied ESP32-compatible SDP munge to the answer")
    return answer


@app.websocket("/ws/femto")
async def femto_ws(websocket: WebSocket):
    """Femto StackChan voice session over a WebSocket (see femto_ws.py).

    The first message is the session metadata (JSON text); after that the
    connection carries PCM audio both ways until the device closes it.
    """
    await websocket.accept()
    try:
        meta = json.loads(await websocket.receive_text())
    except Exception as exc:  # noqa: BLE001
        logger.warning(f"femto ws: bad hello ({exc!r}); closing")
        await websocket.close(code=1003)
        return
    if not isinstance(meta, dict):
        meta = {}
    session_id = f"ws-{uuid.uuid4().hex[:12]}"
    transport = FemtoTransport(websocket, femto_params())
    await run_pipeline(transport, session_id, meta, transport.send_event)


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


def _rotate_wake_samples(directory: str, keep: int) -> None:
    """Keep only the newest `keep` .wav files (and their .json siblings)."""
    try:
        wavs = sorted(
            (p for p in Path(directory).glob("*.wav")),
            key=lambda p: p.stat().st_mtime,
        )
    except OSError:
        return
    for p in wavs[:-keep] if keep > 0 else []:
        for f in (p, p.with_suffix(".json")):
            try:
                f.unlink()
            except OSError:
                pass


# --------------------------------------------------------------------------
# Device logs — the Femto firmware ships its log here in batches, so a bad
# turn can be read afterwards (GET /api/device-logs/<device>/<date>).
# --------------------------------------------------------------------------
_DEVICE_RE = re.compile(r"^[a-z0-9-]{1,32}$")
_DATE_RE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
_device_log_pruned: dict[str, float] = {}


def _prune_device_logs(device_dir: Path) -> None:
    """Delete day files older than DEVICE_LOG_KEEP_DAYS (at most hourly)."""
    now = time.time()
    if now - _device_log_pruned.get(str(device_dir), 0) < 3600:
        return
    _device_log_pruned[str(device_dir)] = now
    cutoff = time.strftime("%Y-%m-%d", time.gmtime(now - DEVICE_LOG_KEEP_DAYS * 86400))
    for f in device_dir.glob("*.log"):
        if f.stem < cutoff:
            f.unlink(missing_ok=True)


@app.post("/api/device-logs")
async def device_logs_upload(request: Request, device: str = "device", boot: str = "-"):
    """Append a batch of device log lines, each prefixed with the arrival
    time (UTC) and the device's boot id."""
    if not DEVICE_LOG_DIR:
        return JSONResponse({"error": "device log storage disabled"}, status_code=503)
    if not _DEVICE_RE.match(device) or not re.match(r"^[0-9a-f-]{1,16}$", boot):
        return JSONResponse({"error": "bad device or boot id"}, status_code=400)
    body = (await request.body())[: 256 * 1024].decode("utf-8", errors="replace")
    day_dir = Path(DEVICE_LOG_DIR) / device
    day_dir.mkdir(parents=True, exist_ok=True)
    _prune_device_logs(day_dir)
    path = day_dir / f"{time.strftime('%Y-%m-%d', time.gmtime())}.log"
    if path.exists() and path.stat().st_size > DEVICE_LOG_MAX_MB_PER_DAY * 1024 * 1024:
        return JSONResponse({"status": "dropped", "reason": "daily cap"}, status_code=202)
    stamp = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    lines = [l for l in body.splitlines() if l.strip()]
    with path.open("a", encoding="utf-8") as f:
        f.writelines(f"{stamp} {boot} {l}\n" for l in lines)
    return {"status": "ok", "lines": len(lines)}


@app.get("/api/device-logs")
async def device_logs_index():
    """Devices and their stored days (newest first), with sizes."""
    if not DEVICE_LOG_DIR or not Path(DEVICE_LOG_DIR).is_dir():
        return {}
    return {
        d.name: [{"date": f.stem, "bytes": f.stat().st_size} for f in sorted(d.glob("*.log"), reverse=True)]
        for d in sorted(Path(DEVICE_LOG_DIR).iterdir())
        if d.is_dir()
    }


@app.get("/api/device-logs/{device}/{date}")
async def device_logs_day(device: str, date: str, tail: int = 0, grep: str = ""):
    """One day's log as text; `tail` keeps the last N lines, `grep` filters
    (case-insensitive substring)."""
    if not DEVICE_LOG_DIR or not _DEVICE_RE.match(device) or not _DATE_RE.match(date):
        return JSONResponse({"error": "not found"}, status_code=404)
    path = Path(DEVICE_LOG_DIR) / device / f"{date}.log"
    if not path.exists():
        return JSONResponse({"error": "not found"}, status_code=404)
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    if grep:
        lines = [l for l in lines if grep.lower() in l.lower()]
    if tail > 0:
        lines = lines[-tail:]
    return PlainTextResponse("\n".join(lines) + "\n")


@app.post("/wake-sample")
async def wake_sample(request: Request):
    """Store a wake-trigger sample uploaded by the device.

    Body = WAV (16 kHz mono int16); query string = the decision metrics
    (seq/peak/avg/hits/win/sr/samples/uptime). Writes <base>.wav + <base>.json
    into WAKE_SAMPLE_DIR (a mounted PVC), rotating the oldest beyond the cap.
    """
    if not WAKE_SAMPLE_DIR:
        return JSONResponse({"error": "wake-sample storage disabled"}, status_code=503)

    body = await request.body()
    if not body:
        return JSONResponse({"error": "empty body"}, status_code=400)

    qp = dict(request.query_params)
    meta = {
        "received_at": time.time(),
        "client": request.client.host if request.client else None,
        "wav_bytes": len(body),
        **qp,
    }

    Path(WAKE_SAMPLE_DIR).mkdir(parents=True, exist_ok=True)
    # Filename: server-receive time (sortable) + the device's seq + uptime, so
    # samples from the same boot stay grouped and nothing collides.
    base = f"{int(time.time())}_seq{qp.get('seq', '0')}_up{qp.get('uptime', '0')}"
    base = "".join(c for c in base if c.isalnum() or c in "._-")
    wav_path = Path(WAKE_SAMPLE_DIR) / f"{base}.wav"
    wav_path.write_bytes(body)
    (Path(WAKE_SAMPLE_DIR) / f"{base}.json").write_text(json.dumps(meta))
    _rotate_wake_samples(WAKE_SAMPLE_DIR, WAKE_SAMPLE_MAX)

    logger.info(
        f"wake-sample stored: {base}.wav ({len(body)} B) "
        f"peak={qp.get('peak')} avg={qp.get('avg')} hits={qp.get('hits')}"
    )
    return JSONResponse({"stored": f"{base}.wav", "bytes": len(body)})


@app.get("/wake-samples")
async def wake_samples_list():
    """List collected wake samples (newest first) with their metrics — so you
    can browse what's been captured without kubectl-ing into the PVC."""
    if not WAKE_SAMPLE_DIR or not Path(WAKE_SAMPLE_DIR).is_dir():
        return JSONResponse({"dir": WAKE_SAMPLE_DIR, "count": 0, "samples": []})
    items = []
    for p in sorted(Path(WAKE_SAMPLE_DIR).glob("*.wav"),
                    key=lambda p: p.stat().st_mtime, reverse=True):
        meta = {}
        j = p.with_suffix(".json")
        if j.exists():
            try:
                meta = json.loads(j.read_text())
            except (OSError, ValueError):
                pass
        items.append({"wav": p.name, "bytes": p.stat().st_size, "meta": meta})
    return JSONResponse({"dir": WAKE_SAMPLE_DIR, "count": len(items), "samples": items})


@app.get("/wake-samples/{name}")
async def wake_sample_get(name: str):
    """Download one collected wake sample (.wav or .json)."""
    if not WAKE_SAMPLE_DIR:
        return JSONResponse({"error": "disabled"}, status_code=503)
    # Defend against path traversal — basename only, must live in the dir.
    safe = Path(name).name
    path = Path(WAKE_SAMPLE_DIR) / safe
    if not path.is_file():
        return JSONResponse({"error": "not found"}, status_code=404)
    media = "audio/wav" if safe.endswith(".wav") else "application/json"
    return FileResponse(path, media_type=media, filename=safe)


class WakeLabelRequest(BaseModel):
    """Body for POST /wake-samples/{name}/label."""

    label: str  # "positive" | "negative" | "unlabeled"


@app.post("/wake-samples/{name}/label")
async def wake_sample_label(name: str, req: WakeLabelRequest):
    """Mark a sample positive (real wake) / negative (false fire) / unlabeled.

    The label is stored in the sample's .json so the training-prep sync
    (collect_hard_negatives.py) can sort clips into the corpus automatically.
    """
    if not WAKE_SAMPLE_DIR:
        return JSONResponse({"error": "disabled"}, status_code=503)
    label = req.label.strip().lower()
    if label not in ("positive", "negative", "unlabeled"):
        return JSONResponse(
            {"error": "label must be positive|negative|unlabeled"}, status_code=400)
    # Resolve to the sample's .json (basename only — no traversal).
    stem = Path(name).name
    stem = stem[:-4] if stem.endswith(".wav") else (
        stem[:-5] if stem.endswith(".json") else stem)
    j = Path(WAKE_SAMPLE_DIR) / f"{stem}.json"
    if not j.is_file():
        return JSONResponse({"error": "not found"}, status_code=404)
    try:
        meta = json.loads(j.read_text())
    except (OSError, ValueError):
        meta = {}
    meta["label"] = label
    j.write_text(json.dumps(meta))
    return JSONResponse({"name": f"{stem}.wav", "label": label})


@app.get("/wake-review")
async def wake_review():
    """Browser UI to play wake samples and label them positive/negative."""
    return FileResponse(APP_DIR / "wake-review.html")


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
