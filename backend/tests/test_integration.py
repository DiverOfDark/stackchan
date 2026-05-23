"""Integration tests that exercise real models and the latency observer.

These tests touch real Whisper (`tiny` to keep CI fast — large-v3-turbo's
800 MB is validated by the unit test on the default string and by the
initContainer at deploy time) and real Piper (the project's configured
Russian voice).

The latency-observer test does NOT load models — it drives the observer
directly with hand-crafted frames so the wiring contract bot.py depends on
is regression-tested without needing a full pipeline run.
"""

from pathlib import Path

import numpy as np
import pytest

from pipecat.frames.frames import (
    BotStartedSpeakingFrame,
    MetricsFrame,
    UserStoppedSpeakingFrame,
    VADUserStartedSpeakingFrame,
    VADUserStoppedSpeakingFrame,
)
from pipecat.metrics.metrics import TTFBMetricsData
from pipecat.observers.base_observer import FramePushed
from pipecat.observers.user_bot_latency_observer import UserBotLatencyObserver
from pipecat.processors.frame_processor import FrameDirection
from pipecat.services.whisper.stt import WhisperSTTService
from pipecat.transcriptions.language import Language


# ---------------------------------------------------------------------------
# Whisper — real model load (tiny, ~75 MB)
# ---------------------------------------------------------------------------


def test_whisper_service_loads_tiny_and_runs_stt():
    """WhisperSTTService must construct with a small real model and run_stt
    must execute end-to-end (returning either a TranscriptionFrame or nothing)
    on a valid silent buffer without raising.

    Guards against:
      - Pipecat changing the WhisperSTTService constructor shape.
      - The faster-whisper run_stt code path silently breaking for the
        Russian language config bot.py uses.
    """
    stt = WhisperSTTService(
        device="cpu",
        compute_type="int8",
        settings=WhisperSTTService.Settings(model="tiny", language=Language.RU),
    )
    assert stt._model is not None, "Whisper model must load"

    # 1 second of silence at Whisper's expected 16 kHz, 16-bit PCM.
    silence_bytes = np.zeros(16000, dtype=np.int16).tobytes()

    import asyncio

    async def _drain():
        # SegmentedSTTService initializes sample_rate from StartFrame at
        # runtime; for this isolated test set it directly so run_stt works.
        stt._sample_rate = 16000
        frames = []
        async for frame in stt.run_stt(silence_bytes):
            if frame is not None:
                frames.append(frame)
        return frames

    # Silence may produce zero frames; we only assert no exception.
    asyncio.run(_drain())


# ---------------------------------------------------------------------------
# Piper — real voice load + synthesis
# ---------------------------------------------------------------------------


def test_piper_voice_loads_and_synthesizes():
    """PiperTTSService must load the configured Russian voice and synthesize
    real PCM audio for a short phrase.

    Guards against:
      - The voice identifier silently changing in the upstream catalog.
      - Piper's synthesis path breaking for Russian.
    """
    from pipecat.services.piper.tts import PiperTTSService

    voice_id = "ru_RU-irina-medium"

    # Stable cache dir so CI's actions/cache can reuse the ~30 MB voice file
    # across runs. Locally this also avoids re-downloading on every invocation.
    cache_dir = Path.home() / ".cache" / "pipecat-tests" / "piper"
    cache_dir.mkdir(parents=True, exist_ok=True)

    tts = PiperTTSService(
        download_dir=cache_dir,
        settings=PiperTTSService.Settings(voice=voice_id),
    )
    assert tts._voice is not None, "Piper voice must load"

    # PiperVoice.synthesize yields AudioChunk-like objects with audio_int16_bytes.
    chunks = list(tts._voice.synthesize("привет"))
    assert chunks, "synthesis must yield at least one chunk"
    total_bytes = sum(len(c.audio_int16_bytes) for c in chunks)
    assert total_bytes > 0, "synthesis must produce non-empty PCM audio"


# ---------------------------------------------------------------------------
# Latency observer wiring — bot.py subscribes to two events; this test
# verifies both fire with the data shape bot.py logs.
# ---------------------------------------------------------------------------


@pytest.fixture
def latency_observer():
    """Observer with capturing handlers attached, mimicking bot.py's wiring."""
    observer = UserBotLatencyObserver()
    captured: dict = {"measured": [], "breakdown": []}

    @observer.event_handler("on_latency_measured")
    async def _on_measured(o, latency_seconds):
        captured["measured"].append(latency_seconds)

    @observer.event_handler("on_latency_breakdown")
    async def _on_breakdown(o, breakdown):
        captured["breakdown"].append(breakdown)

    observer.captured = captured  # type: ignore[attr-defined]
    return observer


async def _push_async(frame, observer):
    await observer.on_push_frame(
        FramePushed(
            source=None,
            destination=None,
            frame=frame,
            direction=FrameDirection.DOWNSTREAM,
            timestamp=0,
        )
    )


async def test_latency_observer_emits_measured_and_breakdown(latency_observer):
    """A full synthetic user turn must trigger both event handlers exactly
    once, with non-None payloads and a populated TTFB breakdown.

    Frame sequence mirrors what a real pipeline emits:
      VADUserStartedSpeaking → VADUserStoppedSpeaking → UserStopped →
      MetricsFrame(TTFB) → BotStartedSpeaking.
    """
    import time

    # 1. User starts speaking → resets per-turn state.
    await _push_async(VADUserStartedSpeakingFrame(timestamp=time.time()), latency_observer)

    # 2. User stops speaking (stop_secs is the VAD silence window).
    stop_ts = time.time()
    await _push_async(
        VADUserStoppedSpeakingFrame(timestamp=stop_ts, stop_secs=0.5),
        latency_observer,
    )

    # 3. The turn closes (STT finalized, turn analyzer released).
    await _push_async(UserStoppedSpeakingFrame(), latency_observer)

    # 4. STT and LLM each report TTFB.
    metrics = MetricsFrame(
        data=[
            TTFBMetricsData(processor="WhisperSTTService", model="tiny", value=0.4),
            TTFBMetricsData(processor="OpenAILLMService", model="hermes", value=0.3),
        ]
    )
    await _push_async(metrics, latency_observer)

    # 5. Bot begins speaking — this is what triggers the observer emits.
    await _push_async(BotStartedSpeakingFrame(), latency_observer)

    # Event handlers run as background tasks scheduled by asyncio.create_task.
    # Wait for them to finish before asserting.
    await latency_observer.cleanup()

    captured = latency_observer.captured  # type: ignore[attr-defined]
    assert len(captured["measured"]) == 1, "on_latency_measured must fire once"
    assert captured["measured"][0] >= 0.0

    assert len(captured["breakdown"]) == 1, "on_latency_breakdown must fire once"
    breakdown = captured["breakdown"][0]
    assert breakdown.user_turn_secs is not None
    # Both TTFB entries must propagate into the breakdown.
    ttfb_processors = {t.processor for t in breakdown.ttfb}
    assert ttfb_processors == {"WhisperSTTService", "OpenAILLMService"}
    # chronological_events() is what bot.py logs; it must produce strings.
    events = breakdown.chronological_events()
    assert all(isinstance(e, str) for e in events)
    assert len(events) >= 3  # user turn + 2 TTFB


async def test_latency_observer_ignores_bot_speech_without_user_turn(latency_observer):
    """A BotStartedSpeakingFrame without a preceding VADUserStopped must NOT
    fire on_latency_measured. (The greeting case still emits the one-time
    first-bot-speech latency, which is fine; this test guards against
    spurious user→bot latency reports.)
    """
    await _push_async(BotStartedSpeakingFrame(), latency_observer)

    captured = latency_observer.captured  # type: ignore[attr-defined]
    assert captured["measured"] == [], "must not emit on_latency_measured"
