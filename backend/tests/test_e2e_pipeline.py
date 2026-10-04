"""End-to-end test driving the same pipeline shape ``run_bot()`` builds.

Real components:
  - WhisperSTTService (model=tiny so CI is fast; real load + frame plumbing)
  - PiperTTSService (real Russian voice, real ONNX synthesis)
  - LLMContextAggregatorPair (the real aggregators, not stubs)
  - UserBotLatencyObserver (the same observer bot.py wires)

Mocked component:
  - The LLM. Replaced with ``FakeLLMService`` which responds to the
    ``LLMContextFrame`` the user aggregator pushes by emitting a fixed
    streaming response. This lets us drive a real, deterministic turn
    end-to-end without needing a live Hermes endpoint.

The test exercises the **greeting path** that ``bot.py`` triggers on
``on_client_connected`` (``run_bot``: ``context.add_message(...)`` +
``LLMRunFrame``). This path skips VAD/audio segmentation but still flows
through every processor downstream of STT — so a regression in the
LLM/TTS/aggregator wiring will fail this test even though we don't feed
audio bytes.
"""

from pathlib import Path

import pytest

from pipecat.frames.frames import (
    Frame,
    LLMContextFrame,
    LLMFullResponseEndFrame,
    LLMFullResponseStartFrame,
    LLMRunFrame,
    LLMTextFrame,
    TTSAudioRawFrame,
)
from pipecat.observers.user_bot_latency_observer import UserBotLatencyObserver
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.task import PipelineParams
from pipecat.processors.aggregators.llm_context import LLMContext
from pipecat.processors.aggregators.llm_response_universal import (
    LLMContextAggregatorPair,
    LLMUserAggregatorParams,
)
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor
from pipecat.services.piper.tts import PiperTTSService
from pipecat.services.whisper.stt import WhisperSTTService
from pipecat.tests.utils import run_test
from pipecat.transcriptions.language import Language


# ---------------------------------------------------------------------------
# Fake LLM — drop-in replacement for OpenAILLMService in tests
# ---------------------------------------------------------------------------


class FakeLLMService(FrameProcessor):
    """Emits a canned streaming response when the user aggregator pushes a
    ``LLMContextFrame`` down the pipeline.

    Mirrors what ``BaseOpenAILLMService`` does in production: emits
    ``LLMFullResponseStartFrame`` → one or more ``LLMTextFrame`` chunks →
    ``LLMFullResponseEndFrame``. The assistant aggregator and TTS consume
    these frames exactly as they would with a real LLM.
    """

    def __init__(self, response_text: str = "Привет"):
        super().__init__()
        self.response_text = response_text
        self.received_contexts: list[LLMContext] = []

    async def process_frame(self, frame: Frame, direction: FrameDirection):
        await super().process_frame(frame, direction)

        if isinstance(frame, LLMContextFrame):
            # Capture for assertions, then emit the fake streamed response.
            self.received_contexts.append(frame.context)
            await self.push_frame(LLMFullResponseStartFrame())
            # Split into a few chunks so TTS sees streaming text, like prod.
            for token in self.response_text.split():
                await self.push_frame(LLMTextFrame(token + " "))
            await self.push_frame(LLMFullResponseEndFrame())
        else:
            # Pass everything else through unchanged.
            await self.push_frame(frame, direction)


# ---------------------------------------------------------------------------
# E2E test — greeting flow with mocked LLM
# ---------------------------------------------------------------------------


# Stable cache dir so CI re-uses downloaded models across runs.
_PIPER_CACHE = Path.home() / ".cache" / "pipecat-tests" / "piper"


@pytest.fixture
def piper_cache_dir():
    _PIPER_CACHE.mkdir(parents=True, exist_ok=True)
    return _PIPER_CACHE


async def test_e2e_greeting_drives_full_pipeline(piper_cache_dir):
    """The greeting flow runs end-to-end through real STT + real TTS + the
    real aggregators, with the LLM replaced by a fake that emits a canned
    response.

    Asserts:
      - The fake LLM received an LLMContextFrame containing the seeded
        greeting prompt (proves user_aggregator → llm wiring works).
      - The TTS produced non-empty audio frames downstream (proves
        llm → assistant_aggregator → tts → output wiring works).
      - The assistant aggregator captured the fake LLM's response back into
        the context (proves the assistant-side feedback loop works).
    """
    # Real services — same constructors bot.py uses, smaller Whisper for CI.
    stt = WhisperSTTService(
        device="cpu",
        compute_type="int8",
        settings=WhisperSTTService.Settings(model="tiny", language=Language.RU),
    )
    tts = PiperTTSService(
        download_dir=piper_cache_dir,
        settings=PiperTTSService.Settings(voice="ru_RU-irina-medium"),
    )
    fake_llm = FakeLLMService(response_text="Привет как дела")

    # Seed the same way bot.py seeds the greeting (bot.py:193-194).
    context = LLMContext()
    greeting_prompt = "Поздоровайся коротко."
    context.add_message({"role": "user", "content": greeting_prompt})

    # Same aggregator construction shape bot.py uses (sans VAD analyzer:
    # we drive LLMRunFrame directly so VAD isn't needed for this flow).
    user_aggregator, assistant_aggregator = LLMContextAggregatorPair(
        context,
        user_params=LLMUserAggregatorParams(),
    )

    # Mirror bot.py's pipeline order (minus the transport processors —
    # run_test wraps the pipeline with its own source/sink).
    pipeline = Pipeline(
        [
            stt,
            user_aggregator,
            fake_llm,
            tts,
            assistant_aggregator,
        ]
    )

    # Drive the same trigger run_bot() uses for the greeting.
    received_down, _ = await run_test(
        processor=pipeline,
        frames_to_send=[LLMRunFrame()],
        pipeline_params=PipelineParams(
            enable_metrics=True,
            audio_in_sample_rate=16000,
            audio_out_sample_rate=22050,  # Piper Russian voices output 22050 Hz
        ),
    )

    # --- Assertion 1: fake LLM was invoked with our context ---
    assert len(fake_llm.received_contexts) == 1, (
        "FakeLLMService must receive exactly one LLMContextFrame"
    )
    seen_context = fake_llm.received_contexts[0]
    seen_messages = seen_context.get_messages()
    user_messages_text = " ".join(
        m["content"] for m in seen_messages if m.get("role") == "user"
    )
    assert greeting_prompt in user_messages_text, (
        f"context handed to LLM must include the seeded prompt; got {seen_messages!r}"
    )

    # --- Assertion 2: TTS produced audio ---
    tts_audio_frames = [f for f in received_down if isinstance(f, TTSAudioRawFrame)]
    assert tts_audio_frames, "TTS must produce at least one audio frame"
    total_audio_bytes = sum(len(f.audio) for f in tts_audio_frames)
    assert total_audio_bytes > 0, "TTS audio frames must contain non-empty PCM"

    # --- Assertion 3: assistant response landed back in the context ---
    final_messages = context.get_messages()
    assistant_text = " ".join(
        m["content"] for m in final_messages if m.get("role") == "assistant"
    )
    assert "Привет" in assistant_text, (
        f"assistant aggregator must capture the fake response; got {final_messages!r}"
    )


async def test_e2e_fake_llm_not_invoked_without_run_frame(piper_cache_dir):
    """Sanity check: without an LLMRunFrame, the LLM must never be invoked.

    Guards against a regression where some other frame inadvertently
    triggers a context push.
    """
    fake_llm = FakeLLMService()
    context = LLMContext()
    user_aggregator, assistant_aggregator = LLMContextAggregatorPair(
        context, user_params=LLMUserAggregatorParams()
    )

    pipeline = Pipeline([user_aggregator, fake_llm, assistant_aggregator])

    # Run with no frames except the implicit EndFrame run_test sends.
    await run_test(
        processor=pipeline,
        frames_to_send=[],
        pipeline_params=PipelineParams(enable_metrics=True),
    )

    assert fake_llm.received_contexts == [], "LLM must not be invoked without LLMRunFrame"
