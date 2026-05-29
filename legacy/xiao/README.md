# pipecat-voice firmware

ESP-IDF firmware for the Seeed **ReSpeaker XVF3800 + XIAO ESP32-S3** dev kit.
Connects to the pipecat backend in `../app/` over WebRTC and acts as a hardware
voice client (mic → STT → LLM → TTS → speaker, with on-chip AEC and
custom-trained Russian wake word).

Architecture plan: `/var/home/diverofdark/.claude/plans/now-let-s-work-on-eager-pond.md`.

## Status

| Milestone | Description | State | Hardware-verified? |
|---|---|---|---|
| M0 | PlatformIO scaffold | done | n/a |
| M1 | I2S loopback through XVF3800 | done | **pending** — ear-test once hardware lands |
| M2 | Wi-Fi SoftAP captive-portal provisioning | done | **pending** |
| M3 | HTTP signaling adapter for `/api/offer` | done | **pending** |
| M4 | One-way mic → backend | done (G.711 µ-law uplink) | verified on hardware |
| M5 | Bidirectional + AEC reference | done (G.711; XVF3800 AEC config TODO — half-duplex echo guard meanwhile) | **pending** |
| M6a | Train Russian wake word offline | trained "Эй, Фемто!" production model (62 KB INT8) | n/a |
| M6b | On-device wake word gating | done — preprocessor + MixedNet + capture gate | **pending** |
| M7 | Mute button + LED state machine | done (LED I2C cmds stubbed) | **pending** |
| M8 | OTA + this README | done | **pending** |
| Backend prep | `ESP32_COMPAT` documented | done | n/a |

Hardware verification is a single user-side milestone: flash, listen, watch
the backend logs. Each module logs enough state that bisection should be
straightforward.

## Build, flash, monitor

```bash
cd firmware
pio run                       # ~30 s incremental build
pio run --target upload       # flash over USB-C
pio device monitor            # 115200 baud serial console
```

PlatformIO downloads ESP-IDF and the toolchain on first run. We pin IDF ≥ 5.3
via `platformio.ini`; if the stock `espressif32` platform falls behind, see
the comment in that file for the pioarduino fork override.

## Hardware

- **Board**: Seeed ReSpeaker XVF3800 (4-mic linear array, AEC/AGC/beamforming on XMOS chip) + XIAO ESP32-S3 host. Item page: <https://www.seeedstudio.com/ReSpeaker-XVF3800-4-Mic-Array-With-XIAO-ESP32S3-p-6489.html>.
- **I2S pins** (fixed by carrier): BCK=8, WS=7, TX=44, RX=43. XVF3800 is I2S master.
- **Audio format on the wire**: 16 kHz, stereo, 32-bit Philips standard.
- **I2C** to XVF3800 for runtime config: pins documented in `xvf3800.c` once that wires in (M5/M7 follow-up).
- **AEC reference**: TTS playback travels through XVF3800 on the channel it treats as AEC ref (left of XVF3800's I2S input). First M5 hardware test confirms exact channel mapping.

## First-time provisioning

On first boot the device exposes a Wi-Fi AP named `pipecat-voice-XXXX` (last
2 bytes of the STA MAC, hex). Connect from a phone/laptop:

1. Join the open AP.
2. Open `http://192.168.4.1/`.
3. Enter Wi-Fi SSID + password + backend URL (e.g. `https://your.backend.example`).
4. Tap "Save & reboot". Device reconnects to the saved network and starts the
   WebRTC session.

If you typo the password and bricks itself into a reconnect loop, hold the
BOOT button on the XIAO for 5 s — `button.c` wipes Wi-Fi creds from NVS and
reboots into provisioning mode.

## Backend prerequisites

Set `ESP32_COMPAT=true` in the backend env. This activates `esp32_munge()` in
`app/bot.py:136` which trims sha-384/sha-512 fingerprints from the SDP answer
(ESP32 mbedTLS does sha-256 only) and rewrites STUNner relay candidates as
host candidates so the minimal embedded SDP parser can chew them. Browser
clients in the same deployment may need re-testing if you flip this in
production — see `config.example.env`.

## Architecture

The firmware is layered C++17. Each layer depends only on the ones below it;
the Domain layer has zero ESP-IDF dependencies and is unit-tested on the
host with Catch2 (see `host_test/README.md`).

```
       ┌── main.cpp ──┐
       │              │
   app::Session  ┄ app::Ui ┄ app::ProvisioningApp (in main.cpp)
       │              │
       └── transport ┄ Peer ┄ Signaling ┄ WakeEngine
                              │
                              ▼
                  ┌── domain (pure C++17, host-testable) ──┐
                  │ LedFsm   SessionFsm   formUrlEncoded   │
                  │ WakeWindow   energy_gate   AudioFrame  │
                  └────────────────────────────────────────┘
                                ▲
                                │
   hal:: ── Xvf3800   AudioIo   Button   NvsKv   Sntp
            WifiSta   SoftApPortal   HttpsClient
```

Rules:
- **App** depends on Transport + Domain; owns FreeRTOS tasks and program lifecycle.
- **Transport** depends on HAL + Domain; wraps libpeer / TFLM /
  esp_http_client behind typed std::function callbacks and RAII destructors.
  (Wire audio is G.711 µ-law, companded in domain::g711 — no codec library.)
- **HAL** depends on ESP-IDF only; one class per silicon resource handle
  (i2c device, i2s channel, GPIO button, NVS handle, Wi-Fi netif, HTTPD server,
  HTTPS client). Move-only, destructor-released.
- **Domain** depends on the C++ standard library only; pure value types and
  state-machine functions. No FreeRTOS, no esp_log, no ESP-IDF headers.
- Layer prefixes: components are `pv_domain` / `pv_hal` / `pv_transport` /
  `pv_app` to avoid colliding with ESP-IDF's own `components/hal/`. Source
  namespaces stay short: `hal::`, `domain::`, `transport::`, `app::`.

## Directory layout

```
firmware/
├── platformio.ini                 # PlatformIO + ESP-IDF config
├── partitions.csv                 # 8 MB flash, 2× 3 MB OTA slots + storage
├── sdkconfig.defaults             # IDF kconfig (PSRAM, DTLS-SRTP, X.509, ...)
├── CMakeLists.txt                 # top-level IDF project
├── components/
│   ├── pv_domain/
│   │   ├── include/domain/        # *.hpp — pure C++17 headers
│   │   ├── src/                   # *.cpp — same TUs compiled by host_test
│   │   └── test/                  # *.cpp — Catch2 cases (host-side only)
│   ├── pv_hal/
│   │   ├── include/hal/           # hal::Xvf3800, AudioIo, Button, NvsKv,
│   │   └── src/                   # Sntp, WifiSta, SoftApPortal, HttpsClient
│   ├── pv_transport/
│   │   ├── include/transport/     # transport::Peer, Signaling,
│   │   └── src/                   # WakeEngine
│   ├── pv_app/
│   │   ├── include/app/           # app::Session, app::Ui
│   │   └── src/
│   ├── libpeer/                   # vendored WebRTC stack (untouched)
│   └── wake_word/                 # TFLite Micro wake-word component
├── host_test/
│   ├── CMakeLists.txt             # cmake host_test/ → ctest
│   └── README.md
├── main/
│   ├── CMakeLists.txt
│   ├── idf_component.yml          # IDF Component Manager manifest
│   ├── main.cpp                   # ~80 lines: init → STA/SoftAP → Session
│   ├── captive_portal_index.html  # provisioning form (source of truth)
│   └── captive_portal_index_html.c# generated byte-array (tools/embed_html.py)
├── tools/
│   ├── embed_html.py
│   └── train_wake_word/           # Piper + microWakeWord training pipeline
└── README.md                      # this file
```

## Host-side tests

```sh
cd firmware
cmake -S host_test -B host_test/build
cmake --build host_test/build -j
ctest --test-dir host_test/build --output-on-failure
```

5 test suites, 34+ Catch2 cases cover the LED state machine, session-state
transitions, wake-word probability window, RMS / peak-abs primitives, and
the URL-encoded form parser. First configure clones Catch2 v3 via FetchContent
(~10 MB); subsequent builds reuse the cache.

## OTA

```c
#include "ota.h"
ota_check_and_update("https://your.host/firmware/firmware.bin");
```

`ota.c` uses `esp_https_ota` with the bundled public-CA cert bundle (so the
URL just has to be on a TLS-terminated host with a real cert; no manual cert
pinning). On success the new image is written to the inactive OTA slot,
otadata flips, the device reboots into the new firmware. Failed downloads
leave the running slot untouched — safe.

There's no built-in trigger UI yet; you'd typically call this from a button
combo, a backend-issued data-channel message, or a periodic check.

## Troubleshooting

**No log output on `pio device monitor`** — reset the board after starting the
monitor; the XIAO ESP32-S3 sometimes reboots into USB DFU instead of CDC.
Press the RESET button.

**SoftAP doesn't appear** — wait 10 s after boot; the AP comes up after NVS is
checked. Make sure `ESP_LOGI(wifi_prov, "SoftAP up: SSID=...")` showed up
during boot.

**`pio run` says component `media_lib_sal` not found** — the deps have drifted;
update `main/idf_component.yml`. We pin `espressif/esp_peer ~1.4` rather than
the full `esp_webrtc` wrapper precisely to avoid that dep tree.

**TLS handshake to backend fails** — check `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE`
is still on in sdkconfig and that the backend cert is signed by a public CA
in the IDF bundle. For self-signed backends, switch from
`crt_bundle_attach = esp_crt_bundle_attach` to a pinned `cert_pem` in
`pipecat_signaling.c`.

**AEC isn't suppressing** — likely a channel-mapping mistake in the M5
playback path. Confirm with `xvf3800_send_cmd(...AEC_ENABLE=0)`; if that
makes self-transcription appear in backend logs, AEC was working but on the
wrong channel.
