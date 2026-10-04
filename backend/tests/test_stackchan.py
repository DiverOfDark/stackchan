"""Femto StackChan integration: persona, usage context, device events."""

import asyncio
from datetime import datetime, timezone
from types import SimpleNamespace

from pipecat.frames.frames import (
    BotStartedSpeakingFrame,
    BotStoppedSpeakingFrame,
    InterimTranscriptionFrame,
    TranscriptionFrame,
    TTSTextFrame,
    UserStartedSpeakingFrame,
    UserStoppedSpeakingFrame,
)
from pipecat.processors.frame_processor import FrameDirection
from pipecat.transcriptions.language import Language

import bot
from pipecat.transports.base_output import BaseOutputTransport

# Stands in for the pipeline's output transport (only isinstance matters).
OUTPUT = object.__new__(BaseOutputTransport)

USAGE = {
    "signed_in": True,
    "ok": True,
    "fetched_at": "2026-10-03T18:20:35Z",
    "session": {"pct": 38, "resets_at": "2026-10-03T20:34:00Z", "window_secs": 18000, "projection_pct": 71},
    "week": {"pct": 61, "resets_at": "2026-10-08T07:00:00Z", "window_secs": 604800, "pace": "over"},
    "limited": False,
    "back_at": None,
}
NOW = datetime(2026, 10, 3, 18, 20, tzinfo=timezone.utc)


def test_usage_context_has_numbers_and_resets():
    text = bot.stackchan_usage_context(USAGE, now=NOW)
    assert "38 percent" in text and "61 percent" in text
    assert "resets in 2 h 14 min" in text
    assert "projected 71 percent" in text
    assert "pace over" in text


def test_usage_context_signed_out_and_missing():
    assert "unavailable" in bot.stackchan_usage_context({"signed_in": False})
    assert "unavailable" in bot.stackchan_usage_context(None)


def test_usage_context_flags_stale_and_limited():
    text = bot.stackchan_usage_context({**USAGE, "ok": False, "limited": True}, now=NOW)
    assert "rate-limited" in text and "stale" in text


def test_system_prompt_uses_name_honorific_and_usage():
    p = bot.stackchan_system_prompt({"name": "Femto", "honorific": "guv"}, USAGE)
    assert "You are Femto" in p and "'guv'" in p
    assert "Live Claude subscription usage" in p
    assert "Russian or English" in p
    # The Russian-only TTS rules must not leak into the bilingual persona.
    assert bot._TTS_FORMATTING_INSTRUCTION not in p


def test_language_from_device_setting():
    assert bot.stackchan_language({"lang": "ru"}) == Language.RU
    assert bot.stackchan_language({"lang": "en"}) == Language.EN
    assert bot.stackchan_language({"lang": "auto"}) is None
    assert bot.stackchan_language({}) is None


def test_event_observer_maps_frames_once():
    sent = []
    obs = bot.StackchanEventObserver(sent.append)
    frames = [
        UserStartedSpeakingFrame(),
        InterimTranscriptionFrame(text="how much", user_id="u", timestamp="t"),
        TranscriptionFrame(text="how much Claude is left", user_id="u", timestamp="t"),
        UserStoppedSpeakingFrame(),
        BotStartedSpeakingFrame(),
        TTSTextFrame(text="Thirty-eight", aggregated_by="word"),
        BotStoppedSpeakingFrame(),
    ]

    async def push_all():
        for f in frames:
            # Each frame passes several processors; it must be sent once.
            for _ in range(3):
                await obs.on_push_frame(SimpleNamespace(frame=f, direction=FrameDirection.DOWNSTREAM, source=OUTPUT))
            # Broadcast frames also have an upstream twin: ignored.
            twin = type(f)(**{k: getattr(f, k) for k in ("text", "user_id", "timestamp", "aggregated_by") if hasattr(f, k)})
            await obs.on_push_frame(SimpleNamespace(frame=twin, direction=FrameDirection.UPSTREAM, source=OUTPUT))

    asyncio.run(push_all())
    assert [m["t"] for m in sent] == [
        "user_started", "user_text", "user_text", "user_stopped", "bot_started", "bot_text", "bot_stopped",
    ]
    assert sent[1] == {"t": "user_text", "text": "how much", "final": False}
    assert sent[2]["final"] is True
    assert sent[5] == {"t": "bot_text", "text": "Thirty-eight"}


def test_bot_text_follows_the_output_transport():
    sent = []
    obs = bot.StackchanEventObserver(sent.append)
    word = TTSTextFrame(text="Тридцать", aggregated_by="word")
    # Leaving the TTS service (seconds before it's spoken): not yet...
    asyncio.run(obs.on_push_frame(SimpleNamespace(frame=word, direction=FrameDirection.DOWNSTREAM, source=object())))
    assert sent == []
    # ...released by the output transport with its audio: now.
    asyncio.run(obs.on_push_frame(SimpleNamespace(frame=word, direction=FrameDirection.DOWNSTREAM, source=OUTPUT)))
    assert sent == [{"t": "bot_text", "text": "Тридцать"}]


def test_event_observer_survives_send_errors():
    def boom(_):
        raise RuntimeError("data channel closed")

    obs = bot.StackchanEventObserver(boom)
    asyncio.run(obs.on_push_frame(SimpleNamespace(frame=BotStartedSpeakingFrame(), direction=FrameDirection.DOWNSTREAM)))


def test_wake_phrase_is_not_a_question():
    for said in ["Эй, Фемто!", "эй фемто", "Hey, filmta", "Hey Femto.", "[chime]", "(laughter)", ""]:
        assert bot.strip_wake_phrase(said) == "", said


def test_wake_phrase_prefix_is_stripped():
    assert bot.strip_wake_phrase("Эй, Фемто, сколько у меня осталось?") == "сколько у меня осталось?"
    assert bot.strip_wake_phrase("Hey Femto what's the time") == "what's the time"
    # A sentence that merely starts with "эй" stays.
    assert bot.strip_wake_phrase("эй ты как дела") == "эй ты как дела"
    assert bot.strip_wake_phrase("Сколько осталось?") == "Сколько осталось?"


class _FakeResp:
    status = 200

    async def __aenter__(self):
        return self

    async def __aexit__(self, *a):
        return False

    async def json(self):
        return {"text": "сколько осталось", "language_code": "rus"}


class _FakeSession:
    def __init__(self):
        self.fields = None

    def post(self, url, data, headers):
        self.fields = {f[0]["name"]: f[2] for f in data._fields}
        return _FakeResp()


async def _stt_fields(language, keyterms):
    session = _FakeSession()
    stt = bot.ScribeSTTService(
        api_key="k",
        aiohttp_session=session,
        settings=bot.ScribeSTTService.Settings(model="scribe_v2", language=language, tag_audio_events=False, keyterms=keyterms),
    )
    result = await stt._transcribe_audio(b"RIFF")
    return session.fields, result


async def test_scribe_auto_language_omits_language_code():
    fields, result = await _stt_fields(None, ["Фемто", "Femto"])
    assert "language_code" not in fields
    assert fields["model_id"] == "scribe_v2"
    assert fields["tag_audio_events"] == "false"
    assert result["text"] == "сколько осталось"


async def test_scribe_fixed_language_is_sent():
    fields, _ = await _stt_fields(Language.RU, None)
    assert fields["language_code"] == "rus"  # ElevenLabs code, converted by the service
