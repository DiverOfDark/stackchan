# pipecat-voice firmware

ESP-IDF firmware for the Seeed **ReSpeaker XVF3800 + XIAO ESP32-S3** dev kit.
Connects to the pipecat backend in `../app/` over WebRTC and acts as a hardware
voice client (mic → STT → LLM → TTS → speaker, with on-chip AEC).

See `/var/home/diverofdark/.claude/plans/now-let-s-work-on-eager-pond.md` for the
full architecture plan.

## Status

| Milestone | Description | Status |
|---|---|---|
| M0 | PlatformIO scaffold, boots and logs chip info | in progress |
| M1 | I2S loopback through XVF3800 | todo |
| M2 | Wi-Fi SoftAP provisioning | todo |
| M3 | HTTP signaling adapter for `/api/offer` | todo |
| M4 | One-way mic → backend | todo |
| M5 | Bidirectional + AEC reference | todo |
| M6a | Train Russian wake word (offline) | todo |
| M6b | On-device wake word gating (microWakeWord + esp-tflite-micro) | todo |
| M7 | LED ring + button polish | todo |
| M8 | OTA + this README finalized | todo |

## Build (PlatformIO)

```bash
# from repo root
cd firmware

# install PlatformIO if you don't have it:
#   pipx install platformio   (or pip install --user platformio)

pio run                            # build
pio run --target upload            # flash over USB-C
pio device monitor                 # serial console at 115200
```

PlatformIO will fetch ESP-IDF and toolchain on first run. We pin IDF >= 5.3 via
`platformio.ini`; if the stock `espressif32` platform falls behind, see the
comment in that file for the pioarduino fork override.

## Hardware

- **Board**: Seeed ReSpeaker XVF3800 (4-mic linear array, AEC/AGC/beamforming on XMOS chip) with XIAO ESP32-S3 host.
- **I2S pins** (fixed by carrier board): BCK=8, WS=7, TX=44, RX=43.
- **I2C** to XVF3800 for runtime config: pins documented in M1 once wired.
- **AEC reference**: TTS playback must travel through XVF3800 on the channel that XVF3800 treats as its AEC reference input (left of its I2S RX). The first M5 test confirms exact channel mapping.

## Provisioning (after M2 lands)

On first boot the device exposes a Wi-Fi AP named `pipecat-voice-XXXX`.
Connect, open `http://192.168.4.1/`, enter Wi-Fi SSID + password + backend URL
(e.g. `https://your.backend.example/api/offer`). Credentials persist in NVS;
long-press the user button to wipe and re-provision.

## Backend prerequisites

The backend in `../app/` already includes `esp32_munge()` in `app/bot.py:136`
that rewrites SDP answers for minimal embedded WebRTC stacks. Set
`ESP32_COMPAT=true` in the backend environment so the answer is digestible by
esp-webrtc-solution.

## Directory layout

```
firmware/
├── platformio.ini        # PlatformIO + ESP-IDF config
├── partitions.csv        # flash layout (single-app for M0..M7, OTA at M8)
├── sdkconfig.defaults    # IDF kconfig defaults (PSRAM, flash size, …)
├── CMakeLists.txt        # top-level IDF project
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml # managed component manifest (per-milestone)
│   └── main.c            # app_main entry
└── (tools/, components/, models/ added in later milestones)
```
