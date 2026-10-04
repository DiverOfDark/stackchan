#include "domain/wake_window.hpp"

namespace domain {

bool WakeWindow::push(float prob, std::chrono::milliseconds now)
{
    if (prob < 0.0f) prob = 0.0f;
    if (prob > 1.0f) prob = 1.0f;
    last_prob_ = prob;

    window_[idx_] = prob;
    idx_ = (idx_ + 1) % kWakeWindowLen;
    if (count_ < kWakeWindowLen) ++count_;

    if (count_ < kWakeWindowLen) {
        // Not enough samples yet for a meaningful average.
        return false;
    }

    float sum = 0.0f;
    for (int i = 0; i < kWakeWindowLen; ++i) sum += window_[i];
    const float avg = sum / static_cast<float>(kWakeWindowLen);

    if (avg < cfg_.threshold) {
        return false;
    }

    if (last_fire_.count() >= 0) {
        if (now - last_fire_ < cfg_.cooldown) {
            return false;
        }
    }

    last_fire_ = now;
    // Damp the window so we don't immediately re-trigger on the same
    // utterance; cooldown is belt-and-suspenders against fast-pulsing.
    window_.fill(0.0f);
    count_ = 0;
    return true;
}

void WakeWindow::reset()
{
    window_.fill(0.0f);
    idx_      = 0;
    count_    = 0;
    last_prob_ = 0.0f;
    last_fire_ = std::chrono::milliseconds{-1};
}

} // namespace domain
