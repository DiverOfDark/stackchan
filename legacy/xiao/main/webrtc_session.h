#pragma once

#include "esp_err.h"

// Glue between esp_peer (WebRTC peer connection) + pipecat_signaling (HTTP
// /api/offer) + audio_io (XVF3800 PCM). Owns the session lifecycle for the
// duration of an active call.
//
// Status: M4 scaffolding.
//   - Build chain is configured (esp_peer + DTLS-SRTP + ECC + cert writing
//     all compile clean).
//   - Signaling vtable adapter: implemented (esp_peer_msg ↔ pipecat_signaling).
//   - Audio capture task: skeleton present, Opus encoding deferred until an
//     encoder dependency is wired in (likely espressif/esp_audio_codec).
//   - Audio playback path: deferred to M5.
//
// First-run plan when hardware lands:
//   1. Verify the signaling round-trip with a no-media offer SDP (M3 already
//      covers this from a libcurl-style call standpoint; here we hand the
//      generated SDP to esp_peer's on_msg callback).
//   2. Add Opus encoder, feed captured PCM, watch backend STT transcribe.
//   3. Tune jitter buffer / DMA sizing if needed.

esp_err_t webrtc_session_start(const char *backend_url);
void      webrtc_session_stop(void);
