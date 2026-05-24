#pragma once

#include "esp_err.h"

// Glue between esp_peer (WebRTC peer connection) + pipecat_signaling (HTTP
// /api/offer) + audio_io (XVF3800 PCM) + wake_word (gating). Owns the
// session lifecycle from boot until shutdown.
//
// On start:
//   - Brings up esp_peer with our esp_peer_default_cfg_t (audio-only, ~16 KB
//     buffers — well under the 400 KB default).
//   - Creates the Opus encoder (16 kHz mono VoIP, 24 kbps, complexity 5,
//     FEC at 10% packet loss) + decoder.
//   - Spawns three tasks pinned to core 1: main_loop (esp_peer_main_loop),
//     capture (I2S → wake_word → optionally Opus → esp_peer_send_audio),
//     playback (stream buffer → Opus decode → I2S with AEC-ref channel
//     mapping).
//
// The capture path always feeds the wake word detector; uplink frames only
// transit while a wake-triggered conversation is active (auto-ends after
// SESSION_IDLE_TIMEOUT_MS of no wake + no inbound TTS).

esp_err_t webrtc_session_start(const char *backend_url);
void      webrtc_session_stop(void);
