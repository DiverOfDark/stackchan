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
