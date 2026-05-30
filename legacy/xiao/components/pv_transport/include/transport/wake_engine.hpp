#pragma once

// C++ facade over the existing TFLite-Micro wake-word component
// (firmware/components/wake_word/wake_word.cc). Forwards init / process /
// detected to the C API for now — Phase E may consolidate the
// threshold + cooldown logic into domain::WakeWindow and have this
// class drive the model only.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace transport {

class WakeEngine {
public:
    // One-shot model load + arena allocation. Idempotent (the underlying
    // C component guards against double-init).
    static esp_err_t initOnce();

    // Push N samples (mono int16, 16 kHz) into the detector.
    static esp_err_t process(const int16_t* pcm, std::size_t n_samples);

    // True if a wake event fired since the last call. Latches; resets on
    // poll.
    static bool detected();

    // Most recent inference probability — useful for diagnostics only.
    static float lastProbability();
};

} // namespace transport
