#pragma once

#include "esp_err.h"

// LED ring driver for the XVF3800 ReSpeaker carrier. The 12 LEDs around the
// mic array are controlled by the XVF3800 itself over I2C (CONTROL_LED_*
// command family in the XMOS programming guide). This module exposes a
// state-machine-friendly API that maps app states to LED patterns.
//
// Status: M7 scaffold. The state-handling and public API are present; the
// I2C write helpers are NOPs pending the XMOS I2C control command table
// being wired in (move from xvf3800.c when that lands in M5/M7 follow-up).

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
