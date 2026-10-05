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
    FEMTO_VOICE_CONNECTING = 1, // woke; connecting to the backend
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

/** Audio that made the wake word fire, for labelling at the backend's
 *  /wake-review (true and false wakes become training data). */
typedef struct {
    uint32_t fire_seq;
    float peak, avg;
    int hits;
    float window[5];
    uint32_t uptime_ms;
} femto_wake_sample_meta_t;

/** Take the newest unread wake snapshot: up to `cap` samples of 16 kHz mono
 *  PCM (the ~3 s ending at the fire) into `out`. Returns the sample count,
 *  0 if there's no new one. */
size_t femto_voice_take_wake_sample(int16_t *out, size_t cap, femto_wake_sample_meta_t *meta);

/** Listen for the wake word or not (e.g. only with someone around). Push-
 *  to-talk works either way. Armed by default. */
void femto_voice_set_wake_armed(bool armed);

void femto_voice_set_volume(int volume);

/** Mic level 0..1 (diagnostics / loud-noise reaction). */
float femto_voice_mic_level(void);

#ifdef __cplusplus
}
#endif
