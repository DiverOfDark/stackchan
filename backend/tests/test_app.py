"""FastAPI lifespan + endpoint smoke tests.

These don't load real models — `_prewarm_whisper` and `_prewarm_piper` are
monkeypatched. The point is to catch wiring regressions in the lifespan,
the endpoints, and the prewarm hook contract.
"""

import importlib

import pytest
from fastapi.testclient import TestClient


@pytest.fixture
def bot_module(monkeypatch):
    """Reload bot.py with the configured WHISPER_MODEL default."""
    monkeypatch.delenv("WHISPER_MODEL", raising=False)
    monkeypatch.delenv("PIPER_VOICE", raising=False)
    import bot

    importlib.reload(bot)
    return bot


@pytest.fixture
def mocked_prewarm(bot_module, monkeypatch):
    """Replace prewarm with recorders so tests don't load real models."""
    calls: dict[str, int] = {"whisper": 0, "piper": 0}

    def fake_whisper(app):
        calls["whisper"] += 1
        app.state.whisper_model = object()

    def fake_piper(app):
        calls["piper"] += 1
        app.state.piper_voice = object()

    monkeypatch.setattr(bot_module, "_prewarm_whisper", fake_whisper)
    monkeypatch.setattr(bot_module, "_prewarm_piper", fake_piper)
    return calls


def test_lifespan_calls_both_prewarms(bot_module, mocked_prewarm):
    """Entering the lifespan context must call both prewarm hooks once."""
    with TestClient(bot_module.app) as client:
        client.get("/health")
    assert mocked_prewarm["whisper"] == 1
    assert mocked_prewarm["piper"] == 1


def test_health_endpoint(bot_module, mocked_prewarm):
    with TestClient(bot_module.app) as client:
        resp = client.get("/health")
    assert resp.status_code == 200
    assert resp.json() == {"status": "ok"}


def test_ice_servers_empty_without_turn_config(bot_module, mocked_prewarm, monkeypatch):
    """Without STUNNER_TURN_URI set, /ice-servers returns an empty list."""
    monkeypatch.setattr(bot_module, "STUNNER_TURN_URI", "")
    with TestClient(bot_module.app) as client:
        resp = client.get("/ice-servers")
    assert resp.status_code == 200
    assert resp.json() == {"iceServers": []}


def test_ice_servers_populated_with_turn_config(bot_module, mocked_prewarm, monkeypatch):
    monkeypatch.setattr(bot_module, "STUNNER_TURN_URI", "turn:1.2.3.4:3478?transport=udp")
    monkeypatch.setattr(bot_module, "STUNNER_TURN_USERNAME", "alice")
    monkeypatch.setattr(bot_module, "STUNNER_TURN_PASSWORD", "s3cret")
    with TestClient(bot_module.app) as client:
        resp = client.get("/ice-servers")
    body = resp.json()
    assert body["iceServers"] == [
        {
            "urls": "turn:1.2.3.4:3478?transport=udp",
            "username": "alice",
            "credential": "s3cret",
        }
    ]


def test_index_serves_test_client_html(bot_module, mocked_prewarm):
    with TestClient(bot_module.app) as client:
        resp = client.get("/")
    assert resp.status_code == 200
    assert "html" in resp.headers["content-type"].lower()


def test_lifespan_tolerates_prewarm_failure(bot_module, monkeypatch):
    """If a prewarm function raises, the app must still start (degraded mode)."""

    def boom(app):
        raise RuntimeError("simulated model load failure")

    monkeypatch.setattr(bot_module, "_prewarm_whisper", boom)
    monkeypatch.setattr(bot_module, "_prewarm_piper", boom)

    with TestClient(bot_module.app) as client:
        resp = client.get("/health")
    assert resp.status_code == 200
