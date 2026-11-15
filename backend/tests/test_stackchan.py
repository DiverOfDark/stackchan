"""Femto StackChan: the Claude usage paragraph of the persona."""

from datetime import datetime, timezone

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
