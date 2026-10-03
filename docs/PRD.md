# PRD: Femto (custom Stack-chan firmware)

| | |
|---|---|
| Status | Draft v0.2 · 2026-10-03 · decisions recorded in §12 |
| Owner | diverofdark |
| Device | M5Stack StackChan (StackChan Core = CoreS3 variant, StackChan Body) |
| Design | `design/Femto Dystopia.dc.html` (variant E, "Dystopian anime") |
| Related repos | [trmnl-cyberpunk](https://github.com/DiverOfDark/trmnl-cyberpunk) (usage data), [pipecat-voice-assistant](https://github.com/DiverOfDark/pipecat-voice-assistant) (voice backend) |

---

## 1. Summary

Femto is custom firmware that turns the M5Stack StackChan into a contemptuous dystopian robot butler (UNIT 07, valet class). It has three jobs:

1. **Show your Claude subscription usage at a glance.** The 5-hour session % and weekly % sit on the status band, and the face's mood reacts to them. Tapping the screen opens "The Ledger", a full-screen usage view.
2. **Watch you.** The camera detects faces, the eyes follow you, and the pan/tilt servos turn the head toward you. With nobody around it falls into Standby.
3. **Talk.** Wake word, then speech goes to the existing pipecat backend (speech-to-text → LLM → text-to-speech), and replies play in character with on-screen captions.

## 2. Goals and non-goals

### Goals
- G1. Pixel-faithful rendering of every screen in the design's screen sheet (15 screens) on the 320×240 panel at ≥ 20 fps.
- G2. Usage data on the device is never more than 6 min old while the backend is healthy. Staleness is shown on screen when the backend is not healthy.
- G3. Voice round trip from wake word to first audio byte: ≤ 2.5 s p50 on a warm connection.
- G4. Face tracking: servos start moving toward a detected face within 300 ms, with no jitter and no servo buzz at rest.
- G5. Zero-PC operation: once provisioned, the device runs from home Wi-Fi plus your k8s-hosted services.
- G6. Over-the-air (OTA) updates, so the device never needs a USB cable after the first flash.
- G7. Setup and every setting are done from a phone browser through the built-in web UI (§6.7). No app to install, and no reflashing just to change config.

### Non-goals (v1)
- NFC (ST25R3916), IR TX/RX, ESP-NOW remote control.
- Compatibility with the stock XiaoZhi firmware or its cloud.
- Codex or any other agent's usage. The trmnl `AgentUsage` struct supports it, so it could come later.
- Multiple users, face *recognition* (who you are), or cloud storage of camera frames.
- Battery-life optimisation beyond basic screen dimming in Standby. The device is assumed to sit on USB power at a desk.

## 3. Character and tone

- **Name:** "Femto" (フェムト), configurable. The wake word in the existing backend is already "Эй, Фемто!". It is the only wake word, for both languages.
- **Language:** all on-screen copy is English. Voice is bilingual: Femto answers in Russian or English, matching the language you spoke.
- **Persona:** a butler who serves the city that ate the empire, and resents you personally. Polite threats, dry contempt, never "happy", only "satisfied".
- **Honorific:** sir / madam / guv (configurable). Used in captions and voice replies.
- **Corporate mode (default on):** "PROPERTY OF ALDGATE DYNAMICS · EMP-0007" footer, a corp mark on the cheek, an employee ID under the eye, and re-badged boot, setup and ledger screens. The corp name is configurable.

## 4. Hardware baseline

Target: the **official M5Stack StackChan kit**. This is confirmed: the owner's unit is the official kit. The only variance left is the LCD controller revision (C or E), which the firmware detects at boot (FR-0).

| Block | Part | Used for |
|---|---|---|
| SoC | ESP32-S3, 16 MB flash, 8 MB PSRAM | everything |
| Display | 2.0" IPS 320×240. Controller **ILI9342C, or ILI9342E on newer units** (M5 note 2026-08-07) | face and all screens |
| Touch | FT6336U | tap screen = Ledger |
| Camera | GC0308 (0.3 MP, DVP) | face detection |
| Audio in | ES7210, 2 mics | wake word, voice uplink |
| Audio out | AW88298 1 W amp | TTS playback |
| Power | AXP2101 PMIC, AW9523B IO expander | rails, backlight, camera and speaker enables |
| Servos | 2× Feetech **SCS0009** serial bus servos on UART (TX GPIO6 / RX GPIO7). Yaw 360°, pitch ~5–85° | head follow |
| Body | PY32L020 IO expander (12× WS2812C LEDs, servo power enable VM_EN), Si12T 3-zone top touch, INA226, 550 mAh battery | LEDs (mood glow), head pat, servo power |
| Other | BMI270 IMU, LTR-553 proximity/light, BM8563 RTC | nice-to-have: auto-brightness, pick-up detection |

## 5. User experience

### 5.1 Screens (from the design's screen sheet)

| # | Screen | Trigger | Key elements |
|---|---|---|---|
| S1 | **Boot** | power on | Mini face that wakes past 50 % progress, giant italic name with red misregistration, フェムト · 監視, 16-block progress bar, terminal log lines ("> calibrating contempt"…), footer "ALDGATE DYNAMICS · ASSET 07" or the FW version |
| S2 | **Connecting to Wi-Fi** | creds saved | Wi-Fi arcs animate, "SEARCHING THE GRID", SSID in large type, "ATTEMPT n/3" (red after the first failure), quip tag |
| S3 | **First-run setup** | no creds / 3 failed attempts | "CONNECT ME. I DON'T ENJOY WAITING.", AP SSID `FEMTO-SETUP` + key, `192.168.4.1`, threat tag, "HOLD POWER 3S = FACTORY WIPE", mini face glancing left |
| S4 | **Face** (default) | normal operation | status band + face + corp footer |
| S5 | **The Ledger** | tap screen (tap again to close; auto-closes after 15 s) | SESSION // 5H and WEEKLY // QUOTA blocks: 40 px misregistered %, 24-cell block meter, reset line, verdict tag |
| S6 | **Listening** | wake word | VU bars at both edges, caption box with **YOU** tag showing the live transcript |
| S7 | **Processing** | end of user turn | gaze up-right, PROC 5-block counter |
| S8 | **Speaking** | TTS playing | mouth flaps in time with audio amplitude, caption box with name tag, text typed out at speech pace |

Each screen except S1–S3 has the **status band**: SESSION % and WEEK %, with 10-segment meters, "RST 2H 14M" and the weekly reset day/time, a mood tag centred in the top band, and a hazard stripe under the band.

### 5.2 Moods (face states)

| Mood label | Internal | Visual signature |
|---|---|---|
| Contempt | neutral | heavy lids, slit pupil, lopsided sneer (default) |
| Satisfied | happy | narrowed eyes, thin fanged grin |
| Amused | excited | wide eyes, pinned pupils, full fanged grin, red sparks |
| Scanning | curious | reticle spins, scan line sweeps the screen, brow raised |
| Alarmed | surprised | needle pupils, glasses drop down the nose, red "!" |
| Standby | sleepy | eyes closed flat, blinking `STANDBY _` cursor, servos centred |
| Rationing | worried | knitted brows, 警告 flashes, constant glitching |
| Processing / Listening / Speaking | — | see S6–S8 |

**Auto-mood rules** (evaluated every frame; the first match wins):
1. A voice state is active → Listening / Processing / Speaking.
2. A transient event mood is active (≤ 3 s) → that mood:
   - Amused after boot completes or after a head pat.
   - Scanning when a new face appears.
   - Alarmed on a sudden loud sound, the device being picked up (IMU), or quota crossing 85 %.
   - Satisfied after answering.
3. Session ≥ 85 % or quota locked → **Rationing**.
4. No face seen for 20 s → **Standby**.
5. Session < 25 % → **Satisfied**.
6. Otherwise → **Contempt**.

**Colour levels for any %:** < 60 % = Bone, 60–84 % = Toxic, ≥ 85 % = accent (Signal red).

### 5.3 Animation rules
- Expression parameters (lid top/bottom per eye, brow inner/outer per eye, mouth width/smile/open/skew, sleep, glasses drop, iris scale, fangs) **ease toward the target at 30 % per tick**, exactly as in the design's `E` table. The table in the design is the spec.
- Gaze: pupils ±9 px, eye sockets ±5 px, mouth ±3 px, driven by the face-tracking vector (−1..1).
- Blink every ~4 s (2 frames). No blink when Satisfied, Amused, Alarmed or in Standby.
- Glitch slice (whole face shifted 7 px plus 3 scan bars) every ~4 s, and on 2 of every 14 ticks while Rationing. Disabled when `fx = off`.
- FX overlay: scanlines (1 px dark every 3 rows) and a radial vignette. Disabled when `fx = off`.
- Red misregistration layer: the ink layer is redrawn in accent colour, offset +2.5 px, at 70 % opacity.

### 5.4 Interaction
| Input | Action |
|---|---|
| Tap screen | toggle Face ⇄ Ledger |
| Top touch: head pat (Si12T, any zone) | Amused for 2 s + a snide line (optional TTS) |
| Top touch: double-tap | push-to-talk, the same as the wake word |
| Wake word "Эй, Фемто" | start a voice turn |
| Power button: long press (AXP2101 long-press IRQ, ~2.5 s) | opens the **Factory wipe** confirm screen: "WIPE ME? PRESS AGAIN." plus a 5 s countdown. A short press within 5 s wipes Wi-Fi, tokens, settings and calibration, then reboots into S3. A tap on the screen or the timeout cancels. |
| Power button: short press | no action, except confirming a wipe. There is no privacy mode. |
| Power button: hold ≥ 10 s | hardware power-off, handled by the AXP2101. The firmware sets the off-threshold to 10 s, so it never collides with the wipe gesture |
| Face appears / moves | Scanning, then follow (eyes plus servos) |

### 5.5 Head motion (servos)
- Pan follows the face's horizontal offset over a ±22° range; tilt follows the vertical offset over ±12° around a neutral pitch.
- Smoothing: critically damped. Deadband ±2° to avoid hunting. Speed limited.
- Servo torque turns off (VM_EN low) after 10 s at rest in Standby, to stop buzzing and save power.
- Processing: the head glances up-right (pan +10°, tilt +6°).
- Scanning: the design's −5° roll can't be done with 2 axes. Substitute a small pan "double-take" wiggle.
- Safety: soft limits stay inside the mechanical range. Every move is clamped. Commands to the servos are rate-limited.

### 5.6 LEDs (12 × WS2812C in the body), nice-to-have
- A dim accent glow normally, pulsing while Listening, breathing while Processing, flickering red while Rationing, off in Standby.

## 6. Functional requirements

### 6.1 Boot and connectivity
- FR-0. At boot, read the LCD controller ID and pick the ILI9342C or ILI9342E init sequence. Configure the AXP2101 power key: long-press IRQ ≈ 2.5 s, hard-off 10 s, IRQs routed to the firmware.
- FR-1. Cold boot shows S1 within 1.5 s of power-on. The boot animation runs for a minimum of 3.5 s and can be skipped once the device is ready.
- FR-2. With saved credentials, the device tries Wi-Fi up to 3 times, showing S2 with the attempt counter. If all 3 fail, it shows S3 while continuing to retry in the background.
- FR-3. Without credentials, it shows S3 and starts a SoftAP `<NAME>-SETUP` with a random WPA2 key shown on screen, plus a captive portal at 192.168.4.1.
- FR-4. The captive portal serves the **same web UI as normal operation** (§6.7), opened in its first-run Setup wizard. It collects:
  - Wi-Fi network (from a scan) and password.
  - Usage API base URL and token.
  - Voice backend URL.
  - Character settings (§6.6).
  - Admin password.

  Everything is stored in NVS. The portal's DNS hijack and SoftAP handling are modelled on the pipecat firmware's `softap_portal`. Its hand-written HTML page is replaced by the web UI.
- FR-5. Time sync over SNTP. The timezone is configurable, defaulting to Europe/Berlin, and is used for reset labels.

### 6.2 Usage data
- FR-6. The device polls `GET {usage_url}/api/stackchan/usage` (§7.1) every 60 s with a Bearer token, using ETag / `If-None-Match`.
- FR-7. Between polls, reset countdowns tick down locally from the absolute `resets_at`.
- FR-8. Stale data (> 6 min old, or the backend reports `ok=false`) gets a dimmed status band plus a `STALE` marker in the Ledger.
- FR-9. Never signed in (`signed_in=false`): the status band shows `-- %`, and the Ledger verdict reads "Nobody has authorised my ledger, sir."
- FR-10. The weekly reset label shows the real day/time from `week_resets_at` (e.g. `RST THU 09:00`). The session label shows `RST 2H 14M`.
- FR-11. Ledger verdicts by the worst of session/week:
  - ≥ 85: "Nearly spent. How predictable."
  - ≥ 60: "You're burning through it, {hon}."
  - otherwise: "Reserves adequate. For now."
  - Locked (`limited=true`): "Locked out until HH:MM. Savour it."

### 6.3 Presence and face tracking
- FR-12. The camera runs at QVGA grayscale or RGB565, 5–10 fps, and feeds on-device face detection (ESP-DL `human_face_detect` or equivalent).
- FR-13. Output: the largest face's centre normalised to −1..1. This drives gaze and servos, and updates `last_seen`.
- FR-14. Frames never leave the device. The camera can be turned off in settings, which also disables Standby-by-absence; it then falls back to proximity sensor and touch.
- FR-15. A "Follow me" setting switches eyes plus servos tracking on or off.

### 6.4 Voice
- FR-16. Wake word detection on the device with microWakeWord TFLite Micro, ported from pipecat firmware's `wake_word` component. It uses the single existing "Эй, Фемто" model for both languages. No English wake word is planned.
- FR-16a. Bilingual voice:
  - STT runs with language auto-detection (ElevenLabs Scribe with no fixed `language_code`). The language is detected from what you say after the wake word.
  - The LLM replies in the language of the user's turn.
  - TTS uses the multilingual `eleven_flash_v2_5` with the same voice for both languages.
  - The `voice_lang` setting (auto / ru / en) can force one language.
- FR-17. On wake: show S6 and connect to the pipecat backend over WebRTC (libpeer, G.722 16 kHz, the same signalling as today). Audio is buffered during the connect, as in the existing firmware.
- FR-18. The backend sends state and text over a **WebRTC data channel** (§7.2). The device drives S6 → S7 → S8 from those events, not from guessing at audio energy.
- FR-19. Captions:
  - The user's live (interim) transcript appears under the YOU tag.
  - The bot's text appears under the name tag, shown sentence by sentence as the TTS speaks it.
  - At most 2 lines of 42 characters, scrolling.
- FR-20. Mouth animation is driven by the playback amplitude envelope.
- FR-21. The turn ends on the backend's `bot_stopped_speaking` plus a 5 s follow-up window, then the device returns to Face with Satisfied for 2.6 s. It hangs up after 15 s of silence.
- FR-22. "How much Claude do I have left?" gets a correct in-character answer using live usage data (§7.2, the usage tool).
- FR-23. Echo: half-duplex mic gating during playback is acceptable for v1. Full AEC (ESP-AFE) is a stretch goal.

### 6.5 Device web UI and OTA
- FR-24. The device is reachable over mDNS as `femto.local` and serves the web UI described in §6.7.
- FR-25. OTA through two A/B app partitions with rollback if the new image fails a health check within 60 s. Optional pull-OTA from a URL served by trmnl-cyberpunk, the same pattern as its TRMNL firmware endpoint.

### 6.6 Settings (mirrors the design's tweak props)
| Key | Values | Default |
|---|---|---|
| `name` | text, ≤ 12 chars | Femto |
| `honorific` | sir / madam / guv | sir |
| `eyewear` | exec glasses / ar half-lens / reticle / none | exec glasses |
| `corp` | on / off | on |
| `corp_name` | text | Aldgate Dynamics |
| `accent` | signal red / toxic green / ice blue | signal red |
| `fx` | on / off (scanlines, vignette, glitch, misregistration) | on |
| `follow` | on / off | on |
| `camera` | on / off | on |
| `brightness` | 10–100 %, plus auto (LTR-553) | auto |
| `volume` | 0–100 % | 60 |
| `tz` | IANA timezone | Europe/Berlin |
| `voice_lang` | auto / ru / en | auto |

### 6.7 Web UI (setup and configuration)

A single static single-page app (SPA), compiled at build time and served by the device's HTTP server. There is no server-side rendering on the device and no CDN at runtime: in AP mode the phone has no internet.

**Tech**
- **Svelte 5 + Vite**, TypeScript. Svelte compiles away its runtime, so a small app stays small.
- Alternatives if Svelte disappoints: Preact + htm, or Solid. Both fit the same budget.
- No UI kit. Hand-written CSS in the design's language: soot/bone/accent palette, condensed type, hazard stripes, Share Tech Mono readouts.
- Fonts are **not bundled**: system condensed or monospace fallbacks only, to protect the size budget. Optional later: subset Barlow Condensed woff2, if it fits under 20 KB.
- Hash-based routing (`#/setup`, `#/settings`, …), so any path the captive portal hits falls back to `index.html`.

**Size and serving**
- Budget: **≤ 80 KB gzipped total** (JS + CSS + HTML + icons).
- The build emits pre-gzipped files. The device serves them with `Content-Encoding: gzip`, `Cache-Control: immutable` for hashed assets, and `no-cache` for `index.html`.
- Assets are embedded in the firmware image (`include_bytes!` from the Rust firmware crate), so the UI and firmware version can never drift apart.
- The CMake build runs `npm run build` (or `pnpm`) in `web/` before compiling. CI fails if the budget is exceeded.

**Pages**

| Route | Purpose | Contents |
|---|---|---|
| `#/setup` | first-run wizard (auto-opens in AP mode) | 1. Wi-Fi: scan list with RSSI, manual SSID, password, "test" → shows join result. 2. Backends: usage URL + token, voice URL, each with a **Test** button that hits the backend from the device and shows status/latency. 3. Character: name, honorific, accent, eyewear, corp. 4. Admin password. 5. Reboot into STA mode, then show `http://femto.local` and the device's IP. |
| `#/` | dashboard | connection state, current mood, usage snapshot (session/week %, resets, staleness), firmware version, uptime. A **live screen mirror** (`/api/screen.bmp` polled at 2 fps, or a downscaled MJPEG) is nice-to-have. |
| `#/settings` | everything in §6.6 | Grouped like the design's tweak panel: Character / Corporate / Display / Behaviour / Audio / Time. Changes apply live: the device re-renders immediately. **Save** persists to NVS; **Revert** reloads it. |
| `#/connections` | network and backends | Wi-Fi (change network, forget), usage API, voice backend, OTA pull URL. Secrets are write-only (shown as `••••` plus "set" or "not set"). |
| `#/motion` | servo calibration | Live sliders for pan/tilt, set centre, set soft limits, invert axis, torque on/off, "nod" test. Stored in NVS. |
| `#/test` | manual triggers | Force any screen (S1–S8) or mood, run the scripted "how much Claude" demo, play a test tone, mic level meter, trigger a wake event. Mirrors the design's "Try it" panel. |
| `#/system` | diagnostics and maintenance | RSSI, heap/PSRAM, fps, task stack high-water marks, servo temp/load, last errors. Live log stream (WebSocket). OTA upload with progress. Reboot. Factory wipe (typed confirmation). Settings export/import as JSON. |

**Device HTTP API** (JSON, all under `/api`, consumed only by the SPA)

| Method + path | Notes |
|---|---|
| `GET /api/status` | device, network, usage, mood, versions |
| `GET/PUT /api/settings` | §6.6 keys. `PUT` is a partial merge, validated on the device, and applied live |
| `POST /api/settings/save` · `POST /api/settings/revert` | persist or discard the live changes |
| `GET /api/wifi/scan` · `PUT /api/wifi` | scan, then set credentials |
| `PUT /api/connections` · `POST /api/connections/test` | backend URLs and tokens, and a reachability test run from the device |
| `GET/PUT /api/motion` · `POST /api/motion/jog` | calibration and live jog |
| `POST /api/test/{screen|mood|demo|tone|wake}` | manual triggers |
| `GET /api/screen.bmp` | framebuffer snapshot (nice-to-have) |
| `POST /api/ota` | streamed firmware upload (raw body) |
| `POST /api/reboot` · `POST /api/factory-reset` | maintenance |
| `POST /api/auth/login` · `POST /api/auth/password` | session cookie |
| `WS /api/ws` | push channel: logs, status deltas, mood changes, mic level |

The OpenAPI-ish schema for this API lives in `web/src/api.ts`, as shared TypeScript types. The device side is checked against it by host tests.

**Auth**
- In AP / first-run mode, no auth is needed: the SoftAP itself is WPA2-protected with the key shown on screen.
- After setup, every `/api` call except `/api/auth/login` and `/api/status` (reduced fields) needs a session cookie (`HttpOnly`, `SameSite=Strict`) obtained with the admin password.
- Login has a rate limit: 5 attempts per minute.
- Factory wipe and OTA also need the password re-entered.

**Developer experience**
- `web/` runs standalone with `npm run dev` against a **mock API** (MSW or a tiny Vite middleware) that simulates the device, including the WebSocket. Mock usage values are taken from the design's sliders.
- `npm run dev -- --device femto.local` proxies `/api` to a real device instead.
- Mobile-first layout, because setup always happens from a phone. Works at 360 px wide. No horizontal scroll.

**Acceptance**
- From a factory-wiped device, a phone can go from joining `FEMTO-SETUP` to a working Femto on home Wi-Fi in **≤ 2 minutes**, without typing any URL. The captive portal auto-opens on iOS and Android.
- All settings round-trip: set in the UI → reboot → still set, and visible on the device.
- Lighthouse on a mid-range phone over the SoftAP: the UI is interactive in ≤ 1.5 s.

## 7. Backend changes required

### 7.1 trmnl-cyberpunk: new usage endpoint
The server already polls `api.anthropic.com/api/oauth/usage` every 300 s with OAuth auto-refresh, and caches the result as `AgentUsage` in `DashData.claude`. The existing `GET /api/agents` lacks reset times, so add:

```
GET /api/stackchan/usage
Authorization: Bearer <STACKCHAN_TOKEN>      # new env var; 401 if set and mismatched
→ 200, ETag
{
  "signed_in": true,
  "ok": true,                          // last fetch succeeded
  "fetched_at": "2026-10-03T09:12:00Z", // last_ok
  "session": { "pct": 38, "resets_at": "2026-10-03T11:26:00Z", "window_secs": 18000, "projection_pct": 71 },
  "week":    { "pct": 61, "resets_at": "2026-10-09T07:00:00Z", "window_secs": 604800, "pace": "over" },
  "limited": false,
  "back_at": null
}
```
- Reuse `session_projection()`, `week_pace()`, `is_limited()` and `back_at()`.
- Document it in utoipa so it shows in `/swagger`.
- Return 503 with `signed_in:false` when not signed in.
- Optional later: the device registers in the existing device registry (by MAC), so it appears on `/devices`.

### 7.2 pipecat-voice-assistant: events, captions and usage awareness
1. **Data channel.** Enable a libpeer SCTP data channel on the device. On the server, emit JSON events from a pipeline observer:
   `user_started_speaking`, `user_stopped_speaking`, `user_transcript {text, final}`, `bot_thinking`, `bot_started_speaking`, `bot_text {text}` (per TTS sentence), `bot_stopped_speaking`, `turn_end`.
   This replaces energy-based state guessing and fixes the known "no end-of-turn signal" issue. Consider RTVI messages if they fit.
2. **Usage tool.** Give the LLM a `get_claude_usage` tool, or inject a usage summary into context on every turn, reading §7.1. Today tools live inside Hermes, so this is either a Hermes tool or bot-side context injection. Bot-side injection is simpler and is preferred.
3. **Persona.** Update `SYSTEM_PROMPT` to the Femto butler (contempt, honorific, short replies, ≤ 2 sentences unless asked). Send `honorific` and `name` from the device in the offer metadata.
4. **Bilingual.**
   - Switch Scribe STT to auto-detect.
   - The persona prompt gets both language variants (the honorific stays English: "sir" is part of the character), plus the rule "reply in the language of the last user turn".
   - Keep `eleven_flash_v2_5`, which is multilingual.
5. **Device identity.** Tag `/api/offer` with a device id, so the transcript SSE and logs are attributable.

## 8. Technical approach

### 8.1 Architecture (independent of language)
```
┌───────────── ESP32-S3 ─────────────────────────────────────────────┐
│ core 1: render task (20–30 fps)                                    │
│   FaceEngine (expression params, easing, gaze) → Scene → 320×240   │
│   RGB565 framebuffer in PSRAM (150 KB, ×2) → DMA SPI to LCD        │
│ core 0: system tasks                                               │
│   NetTask   Wi-Fi, SNTP, mDNS, HTTP+WS (SPA + /api), OTA           │
│   UsageTask poll /api/stackchan/usage → UsageState                 │
│   VisionTask camera → face detect → PresenceState                  │
│   AudioTask I2S full-duplex (ES7210/AW88298), wake word, G.722     │
│   VoiceTask WebRTC (libpeer) + data channel → VoiceState           │
│   MotionTask SCS0009 UART bus, smoothing, torque mgmt              │
│   InputTask touch (FT6336U, Si12T), IMU, proximity                 │
│ StateHub: single mood/state machine; tasks publish events, render  │
│           reads a snapshot each frame                              │
└────────────────────────────────────────────────────────────────────┘
```
- **Rendering.** The design is SVG. Port it to immediate-mode primitives (filled quadratic-Bézier paths, ellipses, polygons, AA lines) drawn into a PSRAM sprite.
- **Pre-rendering.** Static layers (background halftone, hazard stripes, vignette, scanline mask) are pre-rendered once per accent colour.
- **Iris glow.** A pre-baked radial sprite instead of a runtime Gaussian blur.
- **Fonts.** Barlow Condensed (600/800 italic), Share Tech Mono, and a Noto Sans JP 900 subset (フェムト警告監視), converted to anti-aliased bitmap fonts at the sizes in use: 7, 8, 10, 11, 12, 13, 15, 16, 20, 28, 32, 34, 40, 50, 54 px. **The caption font (Barlow Condensed 500, 16 px) also includes Cyrillic**, because Russian transcripts and replies appear in captions. Share Tech Mono has no Cyrillic; no system readout needs it.
- **Golden-image tests.** Render each screen-sheet state on the host, and diff it against PNG rasters exported from the design (§10).

### 8.2 Firmware stack: **Option C (decided 2026-10-03)**

The comparison below is kept as the record of the decision.


The voice path depends on C code that already works in your pipecat firmware:
- libpeer WebRTC, which needs ESP-IDF ≥ 5.2 and does **not** work under plain Arduino.
- The microWakeWord / TFLite-Micro wake word.
- esp32-camera and ESP-DL face detection.

All of them are ESP-IDF components. That one fact shapes every option below.

| | **A. PlatformIO + Arduino + M5Unified** | **B. Pure Rust, no_std (esp-hal + embassy)** | **C. Rust app on esp-idf-svc (std) + C components via FFI** | **D. ESP-IDF C++ + M5Unified/M5GFX as IDF components** |
|---|---|---|---|---|
| Display (ILI9342C/E) | M5GFX ✔ (≥ 0.2.27 for E) | mipidsi has ILI9342C; E needs a custom init | mipidsi, or M5GFX via FFI | M5GFX ✔ |
| AA graphics + fonts | LovyanGFX: AA, VLW smooth fonts, sprites ✔ | embedded-graphics + mplusfonts (AA); no alpha read-back → harder | same as B | same as A ✔ |
| Audio codecs ES7210 / AW88298 | M5Unified ✔ | **no crates**, hand-written (3–6 d) | esp_codec_dev via FFI | M5Unified or esp_codec_dev ✔ |
| Camera GC0308 + face detect | esp32-camera ✔, ESP-DL awkward under Arduino | **no driver**, 1–3 weeks; no face detection | esp32-camera + ESP-DL via FFI (known build friction) | ✔ native |
| SCS0009 servos | StackChan-BSP / SCServo lib ✔ | hand-written (2–4 d, simple protocol) | Rust driver (std UART), easy | port from BSP ✔ |
| PY32 expander, Si12T touch | StackChan-BSP ✔ | reverse-engineer from BSP (3–8 d) | Rust port from BSP | port from BSP ✔ |
| WebRTC (libpeer) | ✘ under plain Arduino (needs Arduino-as-IDF-component) | **✘ none in Rust** | ✔ via FFI | ✔ reuse pipecat `pv_transport` as-is |
| Wake word (microWakeWord) | possible, fiddly | ✘ no realistic path | ✔ via FFI | ✔ reuse `wake_word` component as-is |
| Wi-Fi/TLS/OTA | ✔ mature | esp-radio still **beta** (1.0.0-beta.1, 2026-09); TLS via git-only esp-mbedtls | ✔ IDF mature | ✔ |
| Toolchain health | official PlatformIO stuck on Arduino 2.x; 3.x via the small **pioarduino** fork | esp-hal 1.x stable core, but I2S/camera/PSRAM `unstable` and churning | esp-idf-* crates **community-maintained** | Espressif-maintained ✔ |
| Reuse of your existing code | low | none | medium (C components) | **high** (pv_hal, pv_transport, wake_word, softap_portal) |
| Language joy for you | C++ | Rust ✔✔ | Rust for the app, C underneath | C++ |
| Rough time to v1 | ~6–7 wk | **not viable** for v1 (voice is impossible) | ~8–10 wk | **~5–6 wk** |

**Why C**
- B (pure Rust, no_std) is ruled out: it has no WebRTC, no wake word and no camera driver, and its Wi-Fi stack is beta.
- C keeps Rust for everything that is *ours*, and links the C components that already work.
- It costs about 3–4 weeks more than D. The owner accepted that cost.

**Risks accepted with C, and their mitigations**

| Risk | Mitigation |
|---|---|
| esp-idf-* crates are community-maintained and can lag behind IDF | Pin IDF **v5.5.x** and pin the esp-idf-svc/hal/sys versions in `Cargo.lock`. Upgrade only deliberately. libpeer requires IDF 5.x anyway (it breaks on IDF 6 / mbedTLS 4). |
| bindgen and `extra_components` build friction (esp32-camera, esp-dl) | Spike in M0: get every C component building and linking in week 1, before any feature work. |
| ESP-DL, the microWakeWord wrapper and the libpeer glue are C++ / callback-heavy | Write thin **C shims** (`extern "C"`, plain structs, no callbacks into Rust from ISR context). Data crosses the boundary through FreeRTOS queues or ring buffers that Rust owns. |
| No Rust drivers for SCS0009, PY32 expander, Si12T, ES7210/AW88298 | SCS0009, PY32 and Si12T get small Rust drivers ported from M5's StackChan-BSP. The audio codecs use Espressif's `esp_codec_dev` C component through FFI. |
| M5GFX is C++ and not callable from Rust | Not used. Rust renders into its own RGB565 framebuffer and pushes it through IDF `esp_lcd` (SPI + DMA). See §8.3. |

### 8.3 Rust project layout and responsibilities

```
stackchan/
├─ Cargo.toml                 workspace
├─ crates/
│  ├─ femto-core/             no_std + alloc, pure. Mood/state machine (StateHub), expression table
│  │                          and easing, gaze, usage model + countdowns, settings schema + validation,
│  │                          voice event model. 100 % host-tested.
│  ├─ femto-render/           no_std + alloc, pure. AA scanline rasterizer (quadratic Béziers,
│  │                          ellipses, polygons, thick lines), RGB565 blending, pre-baked layers,
│  │                          bitmap font atlases generated in build.rs (fontdue) from TTFs in assets/.
│  │                          Renders every screen. On the host, it writes PNGs for golden tests.
│  ├─ femto-drivers/          embedded-hal 1.0 drivers: scs0009 (servo bus), py32_io (LEDs, VM_EN),
│  │                          si12t (top touch), plus wrappers around axp2101 / aw9523 / ft6336u / bmi270 crates.
│  └─ femto-sim/              desktop simulator: femto-core + femto-render in a window (minifb),
│                             mouse = face position, keys = moods. Mirrors the design prototype.
├─ firmware/                  bin crate on esp-idf-svc (std). Tasks from §8.1, HTTP/WS server,
│  │                          NVS, OTA, esp_lcd push. Embeds web/dist via include_bytes!.
│  └─ components/             C/C++ IDF components + C shims, built through esp-idf-sys extra_components:
│                             libpeer (vendored from pipecat), wake_word (microWakeWord), esp32-camera,
│                             esp-dl face detect, esp_codec_dev
├─ web/                       Svelte SPA (§6.7)
├─ tools/                     oklch→rgb565 palette gen, design-raster export, flash backup/restore
└─ design/                    source design
```
- **Toolchain:**
  - `espup` (Xtensa Rust), `ldproxy` and `espflash`.
  - `cargo run -p firmware` flashes the device and opens the monitor.
  - `cargo run -p femto-sim` runs the simulator.
- **Rendering budget:** a full-frame redraw at 320×240 must take ≤ 35 ms on the S3 at 240 MHz. If profiling misses that target, fall back to dirty-rect rendering: redraw only the face region and the status band diffs.
- **Concurrency:** one `std::thread` per task from §8.1, pinned with `esp_idf_hal::task::thread::ThreadSpawnConfiguration`. Tasks share state through `femto-core` snapshots, using an `Arc<Mutex<_>>` swap, and send events over `std::sync::mpsc`.

### 8.4 Partitions (16 MB)
nvs 24 KB · otadata 8 KB · phy 4 KB · app0 4 MB · app1 4 MB (web UI ≤ 80 KB gz is embedded in the app image) · model 1 MB (wake word, face detect) · storage (LittleFS: fonts, pre-rendered layers, wake samples) ~6.9 MB.

## 9. Non-functional requirements
- **Performance:** ≥ 20 fps on the face screen with fx on, measured over 60 s. Render task ≤ 45 ms per frame. No dropped audio frames while rendering.
- **Memory:** ≥ 60 KB internal heap free at steady state during an active voice call. PSRAM holds the framebuffers, audio rings and camera frames.
- **Reliability:** 7-day soak with no reboot. Watchdog on every task. Automatic reconnect for Wi-Fi, usage polling and voice.
- **Security:**
  - The usage token and voice URL are stored in NVS. NVS encryption is a stretch goal.
  - The web UI is protected with a password set during setup (session cookie, rate-limited login; §6.7).
  - Camera frames never leave the device.
  - OTA images are SHA-256 verified. Signed OTA is a stretch goal.
- **Privacy:** the mic only streams after the wake word or push-to-talk. Listening is always visible (S6 plus LEDs).
- **Thermal and power:** backlight dims to 20 % in Standby. Servo torque is off at rest.

## 10. Testing
- **Host unit tests** (`cargo test` on the pure crates, which run on x86 with no ESP toolchain): FaceEngine (easing, mood resolution rules, blink/glitch timing), usage parsing, countdown formatting, caption wrapping, settings validation and `/api` handlers (request/response vs `web/src/api.ts` types).
- **Web UI:** Vitest component tests for the wizard and the settings forms against the mock API. One Playwright run of the full setup wizard against the mock. CI enforces the bundle-size budget.
- **Golden-image tests:** render every screen-sheet state on the host to a 320×240 PNG and compare it to the design's rasterised canvases, which can be exported from the "On device" section of the design. Threshold ≤ 3 % pixel diff.
- **Hardware-in-the-loop smoke test:** flash → boot → Wi-Fi → usage fetched → wake-word sample played from a speaker → round trip completes.
- **Mocks:** a backend mock mode (`LOCAL_MODE` in trmnl-cyberpunk already serves fake usage). A device "demo" mode reproduces the design's "▶ Femto, how much Claude do I have left?" scripted flow with no network.

## 11. Milestones
| # | Milestone | Contents | Est. (option C) |
|---|---|---|---|
| M-1 | Stock backup | Full 16 MB flash dump of the factory XiaoZhi firmware, verified restore (§12, D6) | 0.5 day |
| M0 | Toolchain + bring-up | Cargo workspace, espup/espflash, esp-idf-svc on pinned IDF 5.5. **All C components linking** (libpeer, wake_word, esp32-camera, esp-dl, esp_codec_dev). esp_lcd framebuffer push (C/E detect), touch, AXP2101 power key + AW9523, Rust drivers for SCS0009 / PY32 / Si12T, audio loopback, one camera frame | 2 wk |
| M1 | Face engine | femto-core + femto-render: all moods, easing, blink/glitch/fx, status band, Ledger, eyewear/corp/accent variants, demo mode, simulator, golden tests | 2 wk |
| M2 | Connected | SoftAP + captive DNS (S2/S3), NVS settings, SNTP, `/api/stackchan/usage` in trmnl-cyberpunk + device poller, device `/api` + WS, OTA, power-button wipe | 1 wk |
| M2b | Web UI | Svelte SPA: setup wizard, settings, connections, motion calibration, test panel, system/logs/OTA; mock API; embedded build | 1 wk |
| M3 | Presence | ESP-DL face-detect shim → gaze + servo follow, Standby, Scanning, head pat, IMU pick-up → Alarmed | 1.5 wk |
| M4 | Voice | wake_word + libpeer via FFI, data-channel events + captions (incl. Cyrillic), bilingual persona + usage injection in pipecat, mouth sync | 2 wk |
| M5 | Polish | Soak test, perf tuning, LEDs, auto-brightness, docs | 0.5 wk |

Total ≈ 10 weeks.

## 12. Decisions and open questions

### Decisions (2026-10-03)
| # | Topic | Decision |
|---|---|---|
| D1 | Firmware stack | **Option C:** Rust app on esp-idf-svc (std) plus C components via FFI. IDF pinned to 5.5.x (§8.2–8.3). |
| D2 | Hardware | **Official M5Stack StackChan kit** (StackChan Core + Body, SCS0009 servos, PY32 body board). The LCD C/E revision is detected at runtime. |
| D3 | Language | **Screen: English only. Voice: Russian and English**, auto-detected per turn. A single wake word, "Эй, Фемто", is used for both languages (§6.4, §7.2). |
| D4 | Factory wipe | **Power button long-press** → on-screen confirm → short press confirms (§5.4). The design's setup-screen copy changes to "HOLD POWER 3S = FACTORY WIPE". |
| D5 | Usage hosting | **Extend trmnl-cyberpunk** with `GET /api/stackchan/usage` (§7.1). |
| D6 | Stock firmware | **Keep it restorable.** Before the first flash: `espflash read-flash 0 0x1000000 backup/stackchan-stock-<mac>.bin` (or `esptool.py read_flash`), plus a SHA-256. The image is stored outside git (`backup/` is gitignored, because it may contain device keys or credentials). `tools/restore-stock.sh` writes it back with `espflash write-bin 0x0`. M5Burner's stock image is the fallback. |

### Open questions
1. **LCD revision on your unit:** C or E? Not blocking, since the firmware detects it, but it helps M0 planning.

## Appendix A: design constants
- **Palette** (OKLCH from the design → RGB565 at build time):
  - Soot BG `0.12 0.015 20`; panel `0.07 0.01 20`.
  - Bone ink `0.88 0.02 80`; secondary `0.72 0.02 60`; dim `0.3 0.02 20`; sclera `0.2 0.02 20`.
  - Toxic `0.86 0.17 110`.
  - Accents:
    - signal red `a 0.6 0.23 25 / ad 0.38 0.15 25 / al 0.75 0.17 40`
    - toxic green `0.75 0.2 140 / 0.42 0.13 145 / 0.88 0.15 125`
    - ice blue `0.72 0.14 225 / 0.42 0.1 235 / 0.86 0.09 210`
- **Geometry:**
  - Eyes centred at x = 112 / 208, y = 116; half-width 34.
  - Mouth centred at 160, 176.
  - Status band 38 px plus a 4 px hazard stripe.
  - Caption box at y 194–236.
- **Expression table:** the design's `E` object (lid, brow, mouth, skew, sleep, drop, iris scale and fang values per mood) is normative. Copy it verbatim into the face engine.
- **Design tick:** 70 ms. Device timings in "ticks" scale to wall-clock (blink every 55 ticks ≈ 3.9 s; glitch every 60 ticks ≈ 4.2 s).
