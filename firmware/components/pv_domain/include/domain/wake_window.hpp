#pragma once

// Sliding-window wake-word probability detector. Pure logic; lives in
// Domain so the threshold + cooldown rules are exercised by host tests
// rather than empirically tuned on hardware. Fed by the TFLite Micro
// inference step (Transport::WakeEngine), one probability sample per
// inference (~30 ms apart in the streaming MixedNet config).

#include <array>
#include <chrono>
#include <cstdint>

namespace domain {

inline constexpr int kWakeWindowLen = 8;

struct WakeConfig {
    float                   threshold     = 0.10f;    // probability avg over window
    std::chrono::milliseconds cooldown    {2000};     // suppress re-fire window
};

class WakeWindow {
public:
    explicit WakeWindow(WakeConfig cfg = {}) : cfg_(cfg) {}

    // Push one inference output (probability in [0, 1]); returns true if
    // the wake event fires THIS sample (sliding-window average crossed
    // threshold + cooldown elapsed). Caller is responsible for the
    // side-effects (LED flash, conv_active flip).
    bool push(float prob, std::chrono::milliseconds now);

    // The most recent sample, useful for diagnostics. 0.0 before the
    // first push.
    float lastProbability() const { return last_prob_; }

    void reset();

private:
    WakeConfig                            cfg_;
    std::array<float, kWakeWindowLen>     window_{};
    int                                   idx_       = 0;
    int                                   count_     = 0;
    float                                 last_prob_ = 0.0f;
    std::chrono::milliseconds             last_fire_{-1};   // -1 = never
};

} // namespace domain
