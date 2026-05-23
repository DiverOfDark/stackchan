#pragma once

#include "esp_err.h"

// HTTP signaling against the pipecat backend's /api/offer endpoint.
//
//   POST   {base}/api/offer   {"sdp": "...", "type": "offer"}
//                           → {"pc_id": "<uuid>", "sdp": "...", "type": "answer"}
//   PATCH  {base}/api/offer   {"pc_id": "...", "candidates": [
//                                {"candidate": "...", "sdp_mid": "...",
//                                 "sdp_mline_index": N}, ...]}
//                           → {"status": "success"}
//
// Designed to plug into esp-webrtc-solution's signaling_t abstraction in M4,
// but lives as a standalone module so JSON shapes + HTTPS plumbing can be
// validated against the real backend independently.

typedef struct pipecat_signaling pipecat_signaling_t;

typedef struct {
    const char *candidate;        // e.g. "candidate:0 1 UDP 2122...."
    const char *sdp_mid;          // e.g. "0"
    int         sdp_mline_index;
} pipecat_ice_candidate_t;

// `base_url` is the backend root, e.g. "https://pipecat.example.com" — without
// "/api/offer". Stored by reference; must outlive the returned handle (the
// wifi_provision NVS-backed pointer is fine).
pipecat_signaling_t *pipecat_signaling_create(const char *base_url);
void                 pipecat_signaling_destroy(pipecat_signaling_t *sig);

// Sends the local SDP offer. On success, writes the remote SDP answer into a
// freshly allocated buffer at *out_remote_sdp (caller must `free()`) and
// stores the returned pc_id internally for subsequent ICE PATCHes.
esp_err_t pipecat_signaling_send_offer(pipecat_signaling_t *sig,
                                       const char *local_sdp,
                                       char **out_remote_sdp);

// PATCH a batch of trickled ICE candidates. Requires send_offer to have
// completed first (so pc_id is known).
esp_err_t pipecat_signaling_send_ice(pipecat_signaling_t *sig,
                                     const pipecat_ice_candidate_t *cands,
                                     size_t n);

// Returns the pc_id captured from the most recent successful send_offer,
// or NULL if none yet.
const char *pipecat_signaling_pc_id(const pipecat_signaling_t *sig);
