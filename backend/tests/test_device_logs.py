"""Device log upload: append, read back, validation, daily files."""

from fastapi.testclient import TestClient

import bot


def client(tmp_path, monkeypatch):
    monkeypatch.setattr(bot, "DEVICE_LOG_DIR", str(tmp_path))
    return TestClient(bot.app)


def test_upload_and_read_back(tmp_path, monkeypatch):
    c = client(tmp_path, monkeypatch)
    body = "I (100) voice: wake → turn armed\nW (250) voice_ws: link down\n\n"
    r = c.post("/api/device-logs?device=femto&boot=0badf00d", content=body.encode())
    assert r.json() == {"status": "ok", "lines": 2}

    days = c.get("/api/device-logs").json()["femto"]
    assert len(days) == 1
    text = c.get(f"/api/device-logs/femto/{days[0]['date']}").text
    lines = text.strip().splitlines()
    assert lines[0].endswith(" 0badf00d I (100) voice: wake → turn armed")
    assert c.get(f"/api/device-logs/femto/{days[0]['date']}?grep=LINK").text.count("\n") == 1
    assert "wake" not in c.get(f"/api/device-logs/femto/{days[0]['date']}?tail=1").text


def test_rejects_path_tricks(tmp_path, monkeypatch):
    c = client(tmp_path, monkeypatch)
    assert c.post("/api/device-logs?device=../etc&boot=1", content=b"x").status_code == 400
    assert c.get("/api/device-logs/femto/..%2f..%2fpasswd").status_code == 404


def test_disabled_without_dir(monkeypatch):
    monkeypatch.setattr(bot, "DEVICE_LOG_DIR", "")
    assert TestClient(bot.app).post("/api/device-logs?device=femto&boot=1", content=b"x").status_code == 503
