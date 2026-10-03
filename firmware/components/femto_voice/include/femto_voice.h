// Voice for Femto (PRD §6.4): wake word, WebRTC to the pipecat backend,
// CoreS3 mic/speaker. C API for the Rust firmware.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FEMTO_VOICE_IDLE = 0,       // waiting for the wake word
    FEMTO_VOICE_CONNECTING = 1, // woke; WebRTC coming up
    FEMTO_VOICE_LISTENING = 2,  // the user may speak / is speaking
    FEMTO_VOICE_THINKING = 3,   // user stopped, waiting for the reply
    FEMTO_VOICE_SPEAKING = 4,   // TTS playing
} femto_voice_state_t;

/** Bring up audio (I2S0 + ES7210/AW88298 over the legacy I2C driver on
 *  `i2c_port`) and the session. `backend_url` is the pipecat base URL; may
 *  be empty (wake word still runs, nothing connects). */
esp_err_t femto_voice_start(const char *backend_url, int i2c_port, int volume, const char *request_data_json);

/** Next backend event (JSON from the "events" data channel) into `out`.
 *  Returns its length, or 0 when the queue is empty. */
size_t femto_voice_next_event(char *out, size_t cap);

femto_voice_state_t femto_voice_state(void);

/** Speaker envelope, 0..1 (mouth animation). */
float femto_voice_level(void);

/** Push-to-talk: start a turn as if the wake word fired. */
void femto_voice_wake(void);

void femto_voice_set_volume(int volume);

/** Mic level 0..1 (diagnostics / loud-noise reaction). */
float femto_voice_mic_level(void);

#ifdef __cplusplus
}
#endif
