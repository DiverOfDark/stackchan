#pragma once

#include <stdbool.h>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"

// LED ring driver for the XVF3800 ReSpeaker carrier. The 12 LEDs around the
// mic array are driven by the XVF3800 itself via I2C (CONTROL_LED_* command
// family in the XMOS programming guide; constants live in xvf3800.h). This
// module owns the mapping from app state → ring colour/effect AND the
// time-based "what state are we in right now" priority logic for the live
// conversation states (TALKING / THINKING / SPEAKING / LISTENING).

typedef enum {
    LED_STATE_OFF,
    LED_STATE_PROVISIONING,    // SoftAP up, waiting for credentials
    LED_STATE_CONNECTING,      // STA-connecting or signaling handshake
    LED_STATE_NEGOTIATING,     // WebRTC peer past signaling, ICE/DTLS in progress
    LED_STATE_LISTENING,       // session live, waiting for the user to start
    LED_STATE_TALKING,         // local-mic energy → user is speaking now
    LED_STATE_THINKING,        // user just stopped, backend STT/LLM/TTS in flight
    LED_STATE_WAKE_ACK,        // brief flash on wake-word detected
    LED_STATE_SPEAKING,        // TTS playback in progress (inbound audio energy)
    LED_STATE_MUTED,           // mic muted by button
    LED_STATE_ERROR,
} led_state_t;

esp_err_t leds_init(void);

// Set the LED ring to `state`. State changes are skipped while a hold is in
// effect — e.g. a WAKE_ACK flash holds the ring for ~1 s so a subsequent
// LISTENING set from the playback task doesn't overwrite it.
void      leds_set(led_state_t state);

// Inputs to the live-conversation state machine. All ticks are FreeRTOS
// tick units (xTaskGetTickCount). `last_*_tick` fields may be 0, meaning
// "no event yet" — the state machine treats them as far-past.
typedef struct {
    TickType_t now_tick;
    TickType_t last_rx_frame_tick;     // bumped when an energetic inbound TTS frame arrives
    TickType_t last_mic_active_tick;   // bumped when local mic RMS crosses the speech gate
    bool       connected;              // peer connection up and SRTP keying done
    bool       muted;                  // hardware mute switch engaged
} led_session_inputs_t;

// Compute the desired live-conversation LED state from `inputs`, and if it
// differs from the last call's result, push it to the ring (idempotent on
// repeat calls). Call from a dedicated UI task at any rate ≥ a few Hz.
// Returns the resolved state for callers that want to log/inspect it.
//
// Priority (highest first): MUTED > SPEAKING > TALKING > THINKING > LISTENING.
// When !inputs.connected, this is a no-op (the connecting/negotiating LED
// is driven by webrtc_session's peer-state callback directly).
led_state_t leds_session_tick(const led_session_inputs_t *inputs);
