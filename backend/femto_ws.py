"""Femto StackChan voice over a plain WebSocket (`/ws/femto`).

WebRTC needs a TURN relay to reach the pod from the LAN; a WebSocket rides
the same HTTPS ingress as everything else. TCP never drops audio — a lost
packet becomes a short delay, which the device's playback buffer and the
send-ahead below absorb.

Wire format (one session per connection, 16 kHz mono PCM16 little-endian):

  device → server
    text    first message: session metadata, the same JSON the WebRTC
            offer carried as request_data ({"device", "name", "honorific",
            "lang"})
    binary  microphone audio

  server → device
    binary  speech audio, sent up to SEND_AHEAD_S ahead of real time
    text    JSON events: the StackchanEventObserver ones, plus
            {"t": "interrupted"} when the user barges in (drop queued audio)
"""

from __future__ import annotations

import asyncio
import json
import time

from pipecat.frames.frames import (
    Frame,
    InputAudioRawFrame,
    InterruptionFrame,
    OutputAudioRawFrame,
    OutputTransportMessageFrame,
    OutputTransportMessageUrgentFrame,
)
from pipecat.serializers.base_serializer import FrameSerializer
from pipecat.transports.websocket.fastapi import (
    FastAPIWebsocketOutputTransport,
    FastAPIWebsocketParams,
    FastAPIWebsocketTransport,
)

SAMPLE_RATE = 16000
# Speech goes out this far ahead of real time: a cushion in the device's
# playback buffer that hides Wi-Fi retransmit stalls. Barge-in flushes it
# (the "interrupted" event), so it doesn't delay interruptions.
SEND_AHEAD_S = 0.3


class FemtoSerializer(FrameSerializer):
    """Raw PCM as binary frames, events as JSON text frames."""

    async def serialize(self, frame: Frame) -> str | bytes | None:
        if isinstance(frame, OutputAudioRawFrame):
            return frame.audio
        if isinstance(frame, InterruptionFrame):
            return json.dumps({"t": "interrupted"})
        if isinstance(frame, (OutputTransportMessageFrame, OutputTransportMessageUrgentFrame)):
            if self.should_ignore_frame(frame):
                return None
            return json.dumps(frame.message, ensure_ascii=False)
        return None

    async def deserialize(self, data: str | bytes) -> Frame | None:
        if isinstance(data, (bytes, bytearray)) and data:
            return InputAudioRawFrame(audio=bytes(data), sample_rate=SAMPLE_RATE, num_channels=1)
        return None


class _SendAheadOutput(FastAPIWebsocketOutputTransport):
    """Paces audio at real time like the stock transport, but runs up to
    SEND_AHEAD_S ahead of the playback clock instead of exactly on it."""

    async def _write_audio_sleep(self):
        now = time.monotonic()
        if self._next_send_time < now:
            # Idle gap (or the first chunk): restart the clock from now.
            self._next_send_time = now
        self._next_send_time += self._send_interval
        wait = self._next_send_time - now - SEND_AHEAD_S
        if wait > 0:
            await asyncio.sleep(wait)


class FemtoTransport(FastAPIWebsocketTransport):
    def __init__(self, websocket, params: FastAPIWebsocketParams, **kwargs):
        super().__init__(websocket, params, **kwargs)
        self._output = _SendAheadOutput(self, self._client, self._params, name=self._output_name)

    async def send_event(self, msg: dict) -> None:
        """Push a JSON event to the device right away (not paced with audio)."""
        await self._output.send_message(OutputTransportMessageUrgentFrame(message=msg))


def femto_params(**overrides) -> FastAPIWebsocketParams:
    return FastAPIWebsocketParams(
        audio_in_enabled=True,
        audio_out_enabled=True,
        audio_in_sample_rate=SAMPLE_RATE,
        audio_out_sample_rate=SAMPLE_RATE,
        # 20 ms chunks (640 bytes) keep first-audio latency low.
        audio_out_10ms_chunks=2,
        serializer=FemtoSerializer(),
        **overrides,
    )
