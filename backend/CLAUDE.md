# CLAUDE.md — how this project works

Streaming **Russian** voice assistant: a hardware device (ESP32-S3 + Seeed
ReSpeaker XVF3800) talks over WebRTC to an in-cluster **pipecat** backend
(STT → local LLM → TTS). STT/TTS are **provider-switchable**: the default is
ElevenLabs (cloud Scribe STT + custom Agent-Smith voice), with a fully local
no-cloud fallback (FastWhisper STT + Piper TTS). The LLM + signaling + media
relay are always self-hosted in-cluster. See **Backend** below.

> `README.md` (root) and `firmware/README.md` cover setup/quickstart but predate
> recent work and are **stale in places** (they still describe G.711, a
> persistent connection, and the `ru_RU-irina-medium` voice). This file is the
> current source of truth for architecture + dev workflow. When they conflict,
> trust this file or the code.

## Repo layout

- `app/` — pipecat backend (`bot.py`), Whisper wrapper, Dockerfile, tests. Deployed to k8s.
- `firmware/` — ESP-IDF / PlatformIO firmware, layered C++17 (see below).
- `chart/` — Helm chart for the backend.
- Backend image is built+deployed via **ArgoCD** from `main` (image tag `sha-<commit>`).

## The two halves talk like this

```
device (ESP32-S3)  ──HTTPS POST /api/offer──►  Traefik ingress (voice-assistant.kirillorlov.pro :443)  ──►  backend :7860
                   ◄──── SDP answer ──────────
device  ──WebRTC media (G.722/RTP over UDP)──►  STUNner TURN (MetalLB LB 192.168.179.21:3478)  ──relay──►  backend (aiortc)
```

All on-LAN (device, ingress, STUNner are local). Signaling is one HTTPS POST per
connection; media is a relayed WebRTC peer connection (libpeer on device, aiortc
in pipecat).

---

## Audio path & codec — **G.722 wideband**

- **Wire codec: G.722** (16 kHz wideband ADPCM, 64 kbit/s, RTP payload type 9).
  Implemented in `firmware/components/pv_domain/{include/domain/g722.hpp,src/g722.cpp}`
  — a faithful port of SpanDSP's ITU G.722 (LGPL), trimmed to 64k/16k/unpacked.
  Host-tested (`test/test_g722.cpp`): 1 kHz round-trip xcorr ≈ 1.000.
- **Why G.722, not Opus or G.711:**
  - Opus *encode* needs a >120 KB SILK scratch that only fits in (slow) PSRAM and
    ran ~1.5× real-time → stalled capture. Not viable on this board.
  - G.711 worked but is 8 kHz narrowband (telephone quality, muffled speaker).
  - G.722 is cheap ADPCM (encodes+decodes in real time), ~7 kHz audio bandwidth,
    and its 16 kHz uplink is exactly what Whisper wants.
- **Stateful**: hold one `domain::G722Codec` per direction (`g722_enc_` uplink,
  `g722_dec_` downlink) in `Session`. The decoder is reset per connection in
  `buildAndOffer()`; the **encoder is reset at wake** so the buffered head-start
  and live audio stay one continuous stream.
- Same framing as PCMU: 320 samples / 20 ms → **160 bytes**, 8 kHz RTP clock
  (RFC 3551 quirk — G.722's rtpmap says 8000 though it carries 16 kHz). This is
  why libpeer reuses the PCMU packetiser unchanged.
- **Backend sample rates must match**: `bot.py` `TransportParams` pins
  `audio_in_sample_rate=16000` and `audio_out_sample_rate=16000`. A mismatch
  makes pipecat's SmallWebRTC output track drop the TTS → device hears silence.
- **libpeer wiring** for G.722 lives in `firmware/components/libpeer/src/`:
  `CODEC_G722` (peer_connection.h), `SSRC_G722`/`PT_G722` (rtp.h), encoder+decoder
  cases (rtp.c), `sdp_append_g722` (sdp.c), and the device selects it in
  `pv_transport/src/peer.cpp` (`cfg.audio_codec = CODEC_G722`).

## Connection model — **on-demand (connect on wake)**

The wake word runs **locally** on the device and needs no backend, so the device
holds **no** WebRTC connection while idle (dark LED, `/diag connected:false
peer:none`). This deliberately avoids the whole class of persistent-connection
bugs (idle timeout, TURN expiry, dead-session detection, reconnect).

Lifecycle (in `firmware/components/pv_app/src/session.cpp`):
1. **Idle**: no peer. `mainLoopTask` waits; `captureTask` runs only wake detection.
2. **Wake** → `conversation_active_=true`, encoder reset, backlog ring cleared,
   `bot_replied_=false`. `mainLoopTask` sees the flag and calls `buildAndOffer()`
   (creates libpeer peer, POSTs the offer via `Signaling`).
3. **Bring-up (~few s, variable)**: while connecting, `captureTask` **buffers** the
   user's speech (encoded G.722) into a **PSRAM ring** (~8 s) so nothing spoken
   during setup is lost.
4. **Completed** (`onPeerState`): `connected_=true`, turn clock restarted; the
   ring is **flushed** to the peer with mild catch-up pacing (≤3 pkts/20 ms) so
   it doesn't arrive as one burst the jitter buffer would drop, then live audio.
5. **End of turn**: `conversation_active_=false` → `mainLoopTask` tears the peer
   down → back to idle. A follow-up needs another wake word.

**Mid-turn reconnect:** if the path dies while connected — `peer_dead_` (libpeer
reported Failed/Disconnected/Closed) or `media_dead` (no inbound packet for
`kMediaDeadMs`, tracked via `last_rx_pkt_tick_`) — `mainLoopTask` rebuilds the
peer up to `kMaxReconnectsPerTurn` times before giving up the turn.

**Concurrency hazard:** `captureTask` sends on `peer_` (AV core) while
`mainLoopTask` builds/tears it down (main core). `peer_mtx_` guards `peer_`
lifetime — held briefly around `sendAudio` (re-checking `peer_` under the lock)
and around teardown. `peer_->tick()` stays lock-free (same task as teardown).

### Turn timing / deadlines
- `turn_deadline_` ends the turn when it lapses. Pushed by:
  - **Wake / Completed**: `now + kAwaitResponseMs` (15 s) — covers the slow first
    round-trip (connect + flush + STT + LLM + TTS can be ~10–15 s).
  - **User mic energy** (`captureTask`): extends the turn **only while awaiting the
    first reply** (`!bot_replied_`). Once the bot has answered, ambient room noise
    must *not* keep re-arming it — `bot_replied_` (set on first inbound TTS frame,
    reset on wake) gates this. Otherwise the turn stays open indefinitely.
  - **Inbound TTS frame** (`onInboundAudio`): `now + kPostResponseSilenceMs` (5 s)
    — the post-reply window; the device hangs up ~5 s after the audio stops.
- `kConnectTimeoutMs` abandons the turn if bring-up stalls.

## TURN / signaling / ICE
- **STUNner** is the TURN gateway (MetalLB UDP). The device gathers only a relay
  candidate (host gathering skipped when ICE servers are configured — see the
  PATCH comment in `peer_connection.c`) so ICE nominates immediately.
- **libpeer's TURN client is custom** (`agent.c`, "TURN client extensions"):
  Allocate + CreatePermission + Send-indications. Upstream libpeer only does
  Allocate.
- **TURN keepalive**: `peer_connection_loop` re-issues CreatePermission + Refresh
  every 60 s (fire-and-forget; a 438/401 stale-nonce reply is folded back into the
  cached nonce in `agent_recv`). Without this, STUNner expires the permission
  (~5 min) / allocation (~10 min) and media silently dies mid-session. (Relevant
  for long single turns; on-demand turns are usually short.)
- **ICE servers are fetched at boot AND re-fetched lazily.** `main.cpp` fetches
  `/ice-servers` (STUNner TURN creds) once at boot. If that fetch came back empty
  — e.g. the device rebooted *while the backend was restarting* — the device has
  no relay candidate, and since the backend's pod IP isn't LAN-routable, ICE only
  succeeds one way (backend→device) and the connect **times out**. `buildAndOffer()`
  now re-fetches `/ice-servers` whenever the cache is empty, so a bad boot fetch /
  backend restart self-heals on the next wake instead of stranding the device.
  (This was a real connect-timeout: no STUNner allocation, ICE pair was a direct
  pod→LAN pair that only worked one direction.)
- **Cold TLS handshake variance.** A normal LAN client does the full TLS handshake
  to Traefik in ~14 ms, but the device occasionally takes ~20 s cold.
  `HttpsClient::request` logs a per-request `dns / tcp+tls / total` breakdown (log
  tag `https_client: timing`). Possible future fix: keep the signaling DNS/TLS
  connection warm while idle.

## Wake word
- microWakeWord TFLM model ("Эй, Фемто!"), runs locally on the +18 dB mic path
  (`pv_transport/src/wake_engine.*`). Threshold/model tooling in
  `firmware/tools/train_wake_word/`. Detection is independent of any connection.
- **Burst detector + two-gate fire** (`wake_word.cc`): fire when ≥`WAKE_MIN_HITS`
  of the last `WAKE_WINDOW_LEN` frames clear `WAKE_THRESHOLD` (0.60) **AND** the
  window peak ≥`WAKE_PEAK_MIN` (0.80) **AND** the window mean ≥`WAKE_AVG_MIN`
  (0.50). The peak+mean gates were added from real captures to reject *weak*
  false positives (one fails peak, one fails mean). **Strong** false positives
  are metric-identical to real wakes (peak ~0.9+, mean ~0.6), so no gate can
  reject them — that's a model-quality problem, fixed only by retraining.
- **Wake-sample collection loop** (for that retraining): every fire snapshots the
  rolling **3 s** of mic audio that triggered it + its metrics (`session.cpp`
  `kWakeCaptureSamples`); the device serves it locally (`/wake.wav`, `/wake.json`)
  and a low-prio uploader task POSTs it to the backend `/wake-sample`, stored as
  `<base>.wav`+`.json` on a PVC (`WAKE_SAMPLE_DIR`, chart `wakeSamples`).
- **Labeling + retrain loop:** label samples in the browser at backend
  **`/wake-review`** (play + mark positive/negative; stored in each `.json`), then
  `tools/train_wake_word/collect_hard_negatives.py sync --backend <url>` pulls the
  labeled clips into the corpus (negative→`hard_negatives/`, positive→
  `positive_real/`) and `train_production.py` weights the false ones the heaviest
  (`sampling 20 / penalty 4`; needs ≥10 hard negatives or they're skipped). The
  **shipped `wake_word_ru.tflite` is now trained on field hard negatives.** Embed a
  new model with `firmware/tools/embed_tflite.py` → `wake_word_model_data.c`.
  Full recipe in `firmware/tools/train_wake_word/README.md`.
- **Gates are tuned to the model's probability distribution** — after a retrain,
  re-check `WAKE_THRESHOLD`/`WAKE_PEAK_MIN`/`WAKE_AVG_MIN` against real `wake!`
  logs (`verify_model.py --roc`), since a new model scores wakes differently.

## LED ring (`pv_domain/src/led_fsm.cpp` + `pv_app/src/ui.cpp`)
- Driven each playback tick by `resolveLedState(connected, muted, since_rx,
  since_mic, conversation_active)` — a **pure function** of the timeline, so LED
  behavior can be reconstructed from `/diag` samples.
- States → colour/effect: Off=dark, Negotiating=purple breath, Listening=green
  solid, Talking=cyan solid, Thinking=amber breath, WakeAck=white flash,
  Speaking=pink breath, Muted=red. When `!connected` it returns `nullopt` (keeps
  last; connecting LEDs are set by the peer-state callback).
- Hold constants (tuned for the on-demand round-trip): `SPEAKING_HOLD=2500ms`
  (bridge inter-sentence gaps → steady pink), `TALKING_HOLD=1500ms` (bridge speech
  pauses → steady cyan), `THINKING_MAX=15000ms` (stay amber through the whole
  backend round-trip instead of dropping to green mid-wait).

## UI chirps (`pv_domain/include/domain/chirp.hpp`)
- Short synthesized sound effects on the speaker: **Wake** ("online" blip on wake)
  and **End** (on session end). Style is "evil cyberpunk corporate" — a low drone
  bed + a glassy FM-bell **tritone**, click-free (end-fade). Pure header, host-
  testable. Triggered via `chirp_pending_` (set by `captureTask`, played by the
  playback task — the sole I2S writer — so there's no cross-task speaker
  contention). Works off-session (wake/end happen with no peer). Synthesized
  from scratch — **do not** embed copyrighted game audio.

## Backend (`app/bot.py`, pipecat)
- Pipeline: SmallWebRTC in → STT → LLMUserAggregator → OpenAI-compatible LLM →
  TTS → SmallWebRTC out. Silero VAD + turn analyzer for end-of-utterance.
- **STT/TTS are provider-switchable** via `STT_PROVIDER` / `TTS_PROVIDER` env:
  - `elevenlabs` (default): ElevenLabs **Scribe** STT (`scribe_v2`, ru) + **custom
    Agent-Smith voice** TTS (`eleven_flash_v2_5`, `ELEVENLABS_VOICE_ID` set per-deploy
    via chart values, `pcm_16000`). Cloud — needs `ELEVENLABS_API_KEY` (chart ExternalSecret →
    openbao `voice-assistant/elevenlabs` property `api_key`). Scribe needs the
    shared `_aiohttp_session` (lifespan). Voice character: `ELEVENLABS_STABILITY`
    (lower=more menacing) / `ELEVENLABS_SIMILARITY`.
  - `whisper`/`piper`: the local, no-cloud fallback (FastWhisper large-v3-turbo
    INT8, Piper). lifespan only prewarms the local model a chosen provider uses.
- **Persona:** the LLM system prompt (chart `values.yaml systemPrompt`, authoritative
  in-cluster) is an Agent Smith style — calm/cold/measured but still helpful, Russian.
- `idle_timeout_secs=None`: the **device** owns session lifecycle (connect on
  wake, hang up after the turn); the backend must not unilaterally cancel a live
  conversation. (Default 300 s used to kill the persistent connection at 5 min.)
- **No greeting** on connect (`on_client_connected` just logs) — on-demand means
  the user is already speaking when it connects; the LED wake-ack is the cue.
- `ESP32_COMPAT=true` activates `esp32_munge()`: drops sha-384/512 fingerprints
  (device mbedTLS is sha-256 only) and rewrites STUNner relay candidates as host
  candidates. Does **not** touch codec/rtpmap lines.
- Note: Piper model currently loads **per connection** (~1.4 s) — minor latency,
  candidate for a future optimization (load once / share).
- **Wake-sample collection endpoints** (gated on `WAKE_SAMPLE_DIR`, a mounted PVC
  via chart `wakeSamples`): `POST /wake-sample` (device uploads WAV body + metrics
  query → `<base>.wav`+`.json`, rotates beyond `WAKE_SAMPLE_MAX`); `GET
  /wake-samples` (list + metrics, incl. each clip's `label`); `GET
  /wake-samples/{name}` (download); `POST /wake-samples/{name}/label`
  (positive/negative/unlabeled → stored in the `.json`); `GET /wake-review`
  (browser UI to play + label). `/` (test-client) links to it. The page ships in
  the image — Dockerfile copies `*.html`.

## Firmware layering (strict, bottom-up; depend only downward)
- `pv_domain` — pure C++17, **zero ESP-IDF deps**, host-testable with Catch2
  (codecs, FSMs, gates, parsers). Keep its `REQUIRES` list empty.
- `pv_hal` — RAII wrappers around one ESP-IDF resource each (i2s, i2c, gpio, nvs,
  wifi, https client, mdns…).
- `pv_transport` — libpeer Peer, Signaling, OpusCodec(unused now), WakeEngine.
- `pv_app` — `Session` (composes everything + the FreeRTOS tasks), `Ui`,
  `WebServer`. `main/main.cpp` wires it up.
- `firmware/components/libpeer/` is a **vendored + patched** copy (TURN client,
  G.722, ICE host-gather skip). Patches are marked `PATCH(libpeer)` / comments.

## Dev workflow

**Build / flash / monitor** (PlatformIO is not on PATH here):
```bash
cd firmware
~/.platformio/penv/bin/pio run                 # ~3-12 s incremental build
# OTA flash over Wi-Fi (no USB needed) — device must be reachable:
curl -sS -X POST --data-binary @.pio/build/seeed_xiao_esp32s3/firmware.bin \
     -H "Content-Type: application/octet-stream" http://<device-ip>/ota
# device this session: 192.168.178.202 (pipecat-voice.local)
```
- On-demand firmware boots to **idle/disconnected** — that's correct, not a fault.
  Don't wait for `connected:true` at idle.
- **USB recovery flash** (when OTA is dead — e.g. handler-cap regression): the XIAO
  is native-USB → `/dev/ttyACM*` (VID `303A`), *not* `/dev/ttyUSB*` (a CH340 is a
  different device — don't flash it). `~/.platformio/penv/bin/pio run -t upload
  --upload-port /dev/ttyACM0` (hold BOOT while plugging if auto-reset fails).

**Host unit tests** (`firmware/host_test/`): designed for CMake + Catch2, but
`cmake` is **not available** in this environment. Validate domain code by
compiling the unit directly with `g++` against a tiny driver, e.g.:
```bash
g++ -std=gnu++17 -O2 -Icomponents/pv_domain/include \
    /tmp/driver.cpp components/pv_domain/src/g722.cpp -o /tmp/check && /tmp/check
```

**Backend deploy**: commit to `main`, `git push origin main` → ArgoCD builds the
image and syncs. Watch:
```bash
kubectl -n argocd get application voice-assistant -o jsonpath='{.status.sync.status}/{.status.health.status} {.status.sync.revision}{"\n"}'
kubectl -n voice-assistant get deploy voice-assistant -o jsonpath='{.spec.template.spec.containers[0].image}{"\n"}'
kubectl -n voice-assistant logs deploy/voice-assistant --since=5m   # logs only; classifier denies `kubectl exec`
```
Firmware + backend changes that touch the wire format (codec, sample rate) must
deploy together.

## On-device diagnostics (web server on the device)
- `GET /diag` — JSON: uptime, heap/psram, `connected`, `peer_state`,
  `conversation_active`, `muted`, `ms_since_tts`, `ms_since_mic`, `wake_p`,
  `rx_audio_pkts`, `rx_audio_peak`, `rx_audio_max_peak`, task stack free.
  Poll it ~1 Hz to reconstruct a turn (LED, audio, wake) without serial.
- `GET /ws/logs` — WebSocket live console (the device keeps **no** history; attach
  **before** the event you want). No auto-replay. A tiny raw-WS client is enough.
- `GET /wake.wav` / `GET /wake.json` — the audio + metrics of the most recent
  wake fire (for inspecting false positives locally; also auto-uploaded).
- `GET /effect|/state|/resume`, `POST /ota`, `GET /hostname` — LED test, OTA, mDNS
  name. UI in `firmware/main/led_test.html` (LED test + diag + OTA + live console).
- **httpd handler cap:** the server registers ~12 URIs; `max_uri_handlers` is
  raised to 16 and `/ota`+`/ws/logs` are registered **first** — exceeding the cap
  silently drops the *last*-registered handlers (this once knocked out `/ota` and
  made the device un-OTA-able, needing a USB reflash). Keep recovery URIs first.

## Debugging a turn (the reliable recipe)
1. Confirm device idle: `/diag` → `connected:false`.
2. Attach console (`/ws/logs`) **and** start a `/diag` poller (~1 Hz) *before* the turn.
3. User says wake word + question.
4. Read: device `ui: →` transitions + `rx_audio_*` + `wake_p`; backend logs for
   `Transcription` / `Generating chat` / `Finished TTS` / `Bot started speaking`
   and connect/disconnect/ICE.

## Known open items
- **Cold TLS handshake variance** (~20 s outlier) — instrumented (`https_client:
  timing`), root cause TBD (DNS-retry vs Wi-Fi/TLS). Fix likely = warm signaling.
  (The *empty-ICE-servers* connect-timeout cause is fixed — see TURN/ICE.)
- **Wake-word false positives:** strong ones are metric-identical to real wakes
  (gates can't help) — the fix is collecting field hard negatives via `/wake-review`
  and retraining. First hard-negative-trained model is shipped; keep labeling +
  retraining to drive the rate down, and re-tune the gates per new model.
- Piper loads per connection on the backend (latency).
- XVF3800 AEC config still uses a half-duplex echo guard (`kEchoGuardMs`) rather
  than full AEC reference tuning.
- The Agent Smith voice (ElevenLabs Scribe STT + custom-voice TTS) is now the
  default; tune persona via `systemPrompt` and timbre via stability/similarity.
