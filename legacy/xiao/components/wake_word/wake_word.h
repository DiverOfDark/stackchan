#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

// On-device wake word detection. Runs the embedded microWakeWord INT8
// streaming MixedNet model via esp-tflite-micro on captured 16 kHz mono PCM
// frames coming out of audio_io. Caller pushes raw PCM in fixed-size chunks
// and polls wake_word_detected() — when true, the host transitions the app
// state machine into an "active session" mode and starts forwarding frames
// to webrtc_session.
//
// Detection pipeline mirrors microWakeWord's design:
//   raw PCM → 40-feature spectrogram every 10 ms (micro_speech frontend)
//   → streaming model inference every 30 ms (3 frames stride)
//   → sliding probability window → wake event on N consecutive hits over
//     threshold

#define WAKE_WORD_SAMPLE_RATE        16000
#define WAKE_WORD_FRAME_SAMPLES_10MS 160     // 10 ms @ 16 kHz

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t wake_word_init(void);

// Feed N samples (mono int16, 16 kHz) into the detector. N should be a
// multiple of WAKE_WORD_FRAME_SAMPLES_10MS for steady-state operation;
// remainders are buffered internally.
esp_err_t wake_word_process(const int16_t *pcm, size_t n_samples);

// Returns true if a wake event has fired since the last call. Latches the
// "true" state until polled, then resets.
bool wake_word_detected(void);

// Most recent probability the model assigned to "wake" — useful for
// logging/tuning the threshold during M6b verification.
float wake_word_last_probability(void);

#ifdef __cplusplus
}
#endif
