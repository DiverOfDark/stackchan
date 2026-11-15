"""Endpoint smoke tests."""

import json

from fastapi.testclient import TestClient

import bot


def test_health_endpoint():
    with TestClient(bot.app) as client:
        assert client.get("/health").json() == {"status": "ok"}


def test_index_serves_the_live_log_page():
    r = TestClient(bot.app).get("/")
    assert r.status_code == 200
    assert "/api/transcripts/stream" in r.text


def test_femto_endpoint_rejects_bad_hello(monkeypatch):
    called = []

    async def fake_run_live(websocket, session_id, meta):
        called.append(meta)

    monkeypatch.setattr(bot, "run_live", fake_run_live)
    monkeypatch.setattr(bot, "OPENAI_API_KEY", "k")
    with TestClient(bot.app).websocket_connect("/ws/femto") as ws:
        ws.send_text("not json")
        assert ws.receive()["type"] == "websocket.close"
    assert called == []


def test_femto_endpoint_closes_without_an_openai_key(monkeypatch):
    called = []

    async def fake_run_live(websocket, session_id, meta):
        called.append(meta)

    monkeypatch.setattr(bot, "run_live", fake_run_live)
    monkeypatch.setattr(bot, "OPENAI_API_KEY", "")
    with TestClient(bot.app).websocket_connect("/ws/femto") as ws:
        ws.send_text(json.dumps({"device": "femto-stackchan"}))
        assert ws.receive() == {"type": "websocket.close", "code": 1011, "reason": ""}
    assert called == []
