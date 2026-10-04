"""Femto voice over WebSocket: wire format, send-ahead pacing, loopback."""

import json
import time

from fastapi import FastAPI, WebSocket
from fastapi.testclient import TestClient
from pipecat.frames.frames import (
    InputAudioRawFrame,
    InterruptionFrame,
    OutputAudioRawFrame,
    OutputTransportMessageUrgentFrame,
    StartFrame,
)
from pipecat.pipeline.pipeline import Pipeline
from pipecat.pipeline.runner import PipelineRunner
from pipecat.pipeline.task import PipelineParams, PipelineTask
from pipecat.processors.frame_processor import FrameDirection, FrameProcessor

import bot
import femto_ws
from femto_ws import FemtoSerializer, FemtoTransport, femto_params

CHUNK = b"\x01\x02" * 320  # 20 ms of 16 kHz PCM16


async def test_serializer_audio_and_events():
    s = FemtoSerializer()
    assert await s.serialize(OutputAudioRawFrame(audio=CHUNK, sample_rate=16000, num_channels=1)) == CHUNK
    msg = await s.serialize(OutputTransportMessageUrgentFrame(message={"t": "bot_text", "text": "Да, сэр"}))
    assert json.loads(msg) == {"t": "bot_text", "text": "Да, сэр"}
    assert json.loads(await s.serialize(InterruptionFrame())) == {"t": "interrupted"}
    assert await s.serialize(StartFrame()) is None

    frame = await s.deserialize(CHUNK)
    assert isinstance(frame, InputAudioRawFrame)
    assert frame.audio == CHUNK and frame.sample_rate == 16000 and frame.num_channels == 1
    assert await s.deserialize("text from the device") is None
    assert await s.deserialize(b"") is None


async def test_send_ahead_runs_ahead_of_real_time_but_is_bounded():
    out = object.__new__(femto_ws._SendAheadOutput)
    out._send_interval = 0.02
    out._next_send_time = 0
    t0 = time.monotonic()
    # 0.3 s of cushion = 15 chunks go out without waiting...
    for _ in range(15):
        await out._write_audio_sleep()
    assert time.monotonic() - t0 < 0.05
    # ...then it settles to real time: 10 more chunks take ~0.2 s.
    for _ in range(10):
        await out._write_audio_sleep()
    assert 0.15 < time.monotonic() - t0 < 0.35


class _Loopback(FrameProcessor):
    """Mic audio straight back out as speech."""

    async def process_frame(self, frame, direction):
        await super().process_frame(frame, direction)
        if isinstance(frame, InputAudioRawFrame):
            await self.push_frame(OutputAudioRawFrame(audio=frame.audio, sample_rate=16000, num_channels=1))
        else:
            await self.push_frame(frame, direction)


def test_websocket_loopback_audio_and_events():
    app = FastAPI()

    @app.websocket("/ws")
    async def ws(websocket: WebSocket):
        await websocket.accept()
        transport = FemtoTransport(websocket, femto_params())
        task = PipelineTask(
            Pipeline([transport.input(), _Loopback(), transport.output()]),
            params=PipelineParams(idle_timeout_secs=None),
        )

        @transport.event_handler("on_client_connected")
        async def _connected(t, client):
            await transport.send_event({"t": "hello"})

        @transport.event_handler("on_client_disconnected")
        async def _gone(t, client):
            await task.cancel()

        await PipelineRunner(handle_sigint=False).run(task)

    with TestClient(app).websocket_connect("/ws") as ws:
        assert json.loads(ws.receive_text()) == {"t": "hello"}
        ws.send_bytes(CHUNK)
        got = b""
        while len(got) < len(CHUNK):
            got += ws.receive_bytes()
        assert got == CHUNK


def test_femto_endpoint_passes_hello_as_session_metadata(monkeypatch):
    seen = {}

    async def fake_run_pipeline(transport, session_id, request_data, send_event):
        seen.update(session_id=session_id, meta=request_data, transport=transport)
        await send_event({"t": "bot_started"})

    monkeypatch.setattr(bot, "run_pipeline", fake_run_pipeline)
    with TestClient(bot.app).websocket_connect("/ws/femto") as ws:
        ws.send_text(json.dumps({"device": "femto-stackchan", "name": "Femto", "lang": "ru"}))
        assert json.loads(ws.receive_text()) == {"t": "bot_started"}
    assert seen["meta"] == {"device": "femto-stackchan", "name": "Femto", "lang": "ru"}
    assert seen["session_id"].startswith("ws-")
    assert isinstance(seen["transport"], FemtoTransport)


def test_femto_endpoint_rejects_bad_hello(monkeypatch):
    called = []

    async def fake_run_pipeline(*args):
        called.append(args)

    monkeypatch.setattr(bot, "run_pipeline", fake_run_pipeline)
    with TestClient(bot.app).websocket_connect("/ws/femto") as ws:
        ws.send_text("not json")
        try:
            ws.receive_text()
        except Exception:  # noqa: BLE001 — closed by the server, as intended
            pass
    assert not called
