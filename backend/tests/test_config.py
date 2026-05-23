"""Fast unit tests: configuration defaults and import shape."""

import importlib


def test_whisper_default_is_large_v3_turbo(monkeypatch):
    monkeypatch.delenv("WHISPER_MODEL", raising=False)
    import bot

    importlib.reload(bot)
    assert bot.WHISPER_MODEL == "deepdml/faster-whisper-large-v3-turbo-ct2"


def test_whisper_compute_type_default_is_int8(monkeypatch):
    monkeypatch.delenv("WHISPER_COMPUTE_TYPE", raising=False)
    import bot

    importlib.reload(bot)
    assert bot.WHISPER_COMPUTE_TYPE == "int8"


def test_piper_voice_default_is_ru_irina_medium(monkeypatch):
    monkeypatch.delenv("PIPER_VOICE", raising=False)
    import bot

    importlib.reload(bot)
    assert bot.PIPER_VOICE == "ru_RU-irina-medium"


def test_whisper_model_env_override(monkeypatch):
    monkeypatch.setenv("WHISPER_MODEL", "small")
    import bot

    importlib.reload(bot)
    assert bot.WHISPER_MODEL == "small"


def test_pipecat_large_v3_turbo_matches_our_default():
    """The hardcoded default string matches pipecat's Model.LARGE_V3_TURBO.

    Guards against the upstream identifier silently changing.
    """
    from pipecat.services.whisper.stt import Model

    assert Model.LARGE_V3_TURBO.value == "deepdml/faster-whisper-large-v3-turbo-ct2"


def test_latency_observer_import_path_stable():
    """The pipecat module path used by bot.py is still valid."""
    from pipecat.observers.user_bot_latency_observer import UserBotLatencyObserver

    observer = UserBotLatencyObserver()
    # bot.py registers handlers for both events; they must exist.
    assert "on_latency_measured" in observer._event_handlers
    assert "on_latency_breakdown" in observer._event_handlers


# ---------------------------------------------------------------------------
# Narration instruction wiring (bot.py SYSTEM_PROMPT composition)
# ---------------------------------------------------------------------------


def test_narration_present_in_default_prompt(monkeypatch):
    """Default SYSTEM_PROMPT must include the tool-narration instruction.

    Why this matters: Hermes' agent loop adds seconds of tool-running latency
    per turn. Without narration the user hears silence until the final answer;
    with narration the model emits one short sentence before each tool call
    that TTS speaks immediately. Regression here = silent voice during agent
    work.
    """
    monkeypatch.delenv("SYSTEM_PROMPT", raising=False)
    import bot

    importlib.reload(bot)
    assert bot._DEFAULT_SYSTEM_PROMPT in bot.SYSTEM_PROMPT
    assert bot._NARRATION_INSTRUCTION in bot.SYSTEM_PROMPT
    # Spot-check Russian content so a future "translate to English" mistake
    # is loud in CI.
    assert "Сейчас проверю погоду" in bot.SYSTEM_PROMPT


def test_narration_always_appended_even_when_prompt_overridden(monkeypatch):
    """The narration instruction is intentionally unconditional — even when an
    operator overrides SYSTEM_PROMPT via env, narration is still appended.
    Perceived-latency in voice is too important to leave up to whoever sets
    the env var remembering to include it.
    """
    monkeypatch.setenv("SYSTEM_PROMPT", "Custom prompt only.")
    import bot

    importlib.reload(bot)
    assert "Custom prompt only." in bot.SYSTEM_PROMPT
    assert bot._NARRATION_INSTRUCTION in bot.SYSTEM_PROMPT


# ---------------------------------------------------------------------------
# Streaming completions are on (pipecat default)
# ---------------------------------------------------------------------------


def test_openai_llm_service_uses_streaming():
    """Sanity check that pipecat's OpenAILLMService produces stream=True in
    its chat-completion params. bot.py relies on this so Hermes' mid-turn
    narration deltas reach TTS as they arrive.

    If pipecat ever flips this default (or moves the knob), this test fails
    loudly — better than discovering silence at runtime.
    """
    from pipecat.services.openai.llm import OpenAILLMService

    llm = OpenAILLMService(
        api_key="test",
        base_url="http://example.invalid/v1",
        settings=OpenAILLMService.Settings(model="hermes-agent"),
    )

    # Minimal OpenAILLMInvocationParams shape: just messages, no tools.
    from pipecat.adapters.services.open_ai_adapter import OpenAILLMInvocationParams

    invocation_params: OpenAILLMInvocationParams = {
        "messages": [{"role": "user", "content": "hi"}],
        "tools": [],
        "tool_choice": None,
    }
    params = llm.build_chat_completion_params(invocation_params)

    assert params.get("stream") is True, (
        "OpenAILLMService must produce stream=True; otherwise narration "
        "deltas can't reach TTS until the LLM finishes the whole turn"
    )
    # Usage chunk is required for the latency observer's TTFB metrics.
    assert params.get("stream_options", {}).get("include_usage") is True
