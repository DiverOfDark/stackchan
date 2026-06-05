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

// Decision metrics for one wake fire — a C++ mirror of the C component's
// wake_word_metrics_t, so callers in pv_app don't pull in the C header.
struct WakeMetrics {
    uint32_t fire_seq = 0;          // increments per fire; 0 = none since boot
    float    peak     = 0.0f;       // max frame prob in the window
    float    avg      = 0.0f;       // mean frame prob over the window
    int      hits     = 0;          // frames over the per-frame threshold
    float    window[5] = {0};       // the window probabilities at fire
};

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

    // Snapshot of the most recent wake fire's decision metrics (peak/avg/hits/
    // window + a fire_seq that increments per fire). For diagnostics and
    // hard-negative capture.
    static WakeMetrics lastMetrics();
};

} // namespace transport
