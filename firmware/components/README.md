# Firmware C/C++ components

Copied from the XIAO voice firmware, which now lives in `legacy/xiao/`
(formerly the pipecat-voice-assistant repo). The wake-word model is built
from `tools/wake-word/` (`embed.py`).

| Component | Origin | Notes |
|---|---|---|
| `femto_vision` | new | esp32-camera + ESP-DL face detection shim |
| `femto_voice` | new, adapted from pipecat-voice-assistant `pv_app/session.cpp` | CoreS3 audio (ES7210/AW88298), voice session, C API for Rust |
| `libpeer` | pipecat-voice-assistant @ f9fd4e7 (patched aconchillo/libpeer) | unchanged |
| `pv_domain` | pipecat-voice-assistant @ f9fd4e7 | unchanged (tests: `legacy/xiao/host_test`) |
| `pv_transport` | pipecat-voice-assistant @ f9fd4e7 | unchanged |
| `pv_hal` | pipecat-voice-assistant @ f9fd4e7 | only `https_client` kept |
| `wake_word` | pipecat-voice-assistant @ f9fd4e7 | "Эй, Фемто" microWakeWord model |

Changes to copied components should go upstream first.

Local changes: `pv_transport` opens a string data channel ("events") and can send `request_data` with the offer (Femto device identity).
