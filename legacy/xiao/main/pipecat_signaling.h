#pragma once

#include "esp_err.h"

// HTTP signaling against the pipecat backend's /api/offer endpoint.
//
//   POST {base}/api/offer  {"sdp": "...", "type": "offer"}
//                        → {"pc_id": "<uuid>", "sdp": "...", "type": "answer"}
//
// Note: libpeer does non-trickle ICE — all candidates are inlined into the
// initial offer SDP — so there's no PATCH / trickle path on the device side.
// (The backend still publishes a /api/offer PATCH endpoint for browser
// clients that trickle.)

typedef struct pipecat_signaling pipecat_signaling_t;

// `base_url` is the backend root, e.g. "https://pipecat.example.com" — without
// "/api/offer". Stored by reference; must outlive the returned handle (the
// wifi_provision NVS-backed pointer is fine).
pipecat_signaling_t *pipecat_signaling_create(const char *base_url);
void                 pipecat_signaling_destroy(pipecat_signaling_t *sig);

// Sends the local SDP offer. On success, writes the remote SDP answer into a
// freshly allocated buffer at *out_remote_sdp (caller must `free()`).
esp_err_t pipecat_signaling_send_offer(pipecat_signaling_t *sig,
                                       const char *local_sdp,
                                       char **out_remote_sdp);

// One ICE server entry fetched from the backend's /ice-servers endpoint.
// The strings are heap-allocated; free the whole array with
// pipecat_signaling_free_ice_servers when esp_peer no longer references them.
typedef struct {
    char *url;       // e.g. "turn:stunner.example.com:3478?transport=udp"
    char *username;
    char *credential;
} pipecat_ice_server_t;

// GET ${base}/ice-servers and parse the response into a freshly-allocated
// array. Sets *out_servers to NULL and *out_count to 0 on a 200 with no
// entries (backend without TURN configured — degrades to host-candidate
// ICE only). Caller owns the returned array.
esp_err_t pipecat_signaling_fetch_ice_servers(pipecat_signaling_t *sig,
                                              pipecat_ice_server_t **out_servers,
                                              size_t *out_count);

void pipecat_signaling_free_ice_servers(pipecat_ice_server_t *servers,
                                        size_t count);
