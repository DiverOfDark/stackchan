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
from pipecat.transcriptions.language import Language

import bot

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
                await obs.on_push_frame(SimpleNamespace(frame=f))

    asyncio.run(push_all())
    assert [m["t"] for m in sent] == [
        "user_started", "user_text", "user_text", "user_stopped", "bot_started", "bot_text", "bot_stopped",
    ]
    assert sent[1] == {"t": "user_text", "text": "how much", "final": False}
    assert sent[2]["final"] is True
    assert sent[5] == {"t": "bot_text", "text": "Thirty-eight"}


def test_event_observer_survives_send_errors():
    def boom(_):
        raise RuntimeError("data channel closed")

    obs = bot.StackchanEventObserver(boom)
    asyncio.run(obs.on_push_frame(SimpleNamespace(frame=BotStartedSpeakingFrame())))


def test_wake_phrase_is_not_a_question():
    for said in ["Эй, Фемто!", "эй фемто", "Hey, filmta", "Hey Femto.", "[chime]", "(laughter)", ""]:
        assert bot.strip_wake_phrase(said) == "", said


def test_wake_phrase_prefix_is_stripped():
    assert bot.strip_wake_phrase("Эй, Фемто, сколько у меня осталось?") == "сколько у меня осталось?"
    assert bot.strip_wake_phrase("Hey Femto what's the time") == "what's the time"
    # A sentence that merely starts with "эй" stays.
    assert bot.strip_wake_phrase("эй ты как дела") == "эй ты как дела"
    assert bot.strip_wake_phrase("Сколько осталось?") == "Сколько осталось?"
