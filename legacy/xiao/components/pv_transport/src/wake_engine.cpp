#include "transport/wake_engine.hpp"

// Forward to the C component. The header is C, so wrap with extern "C".
extern "C" {
#include "wake_word.h"
}

namespace transport {

esp_err_t WakeEngine::initOnce()           { return wake_word_init(); }
esp_err_t WakeEngine::process(const int16_t* pcm, std::size_t n) {
    return wake_word_process(pcm, n);
}
bool      WakeEngine::detected()           { return wake_word_detected(); }
float     WakeEngine::lastProbability()    { return wake_word_last_probability(); }
WakeMetrics WakeEngine::lastMetrics() {
    wake_word_metrics_t c;
    wake_word_get_metrics(&c);
    WakeMetrics m;
    m.fire_seq = c.fire_seq;
    m.peak     = c.peak;
    m.avg      = c.avg;
    m.hits     = c.hits;
    for (int i = 0; i < WAKE_WORD_WINDOW_LEN && i < 5; ++i) m.window[i] = c.window[i];
    return m;
}

} // namespace transport
