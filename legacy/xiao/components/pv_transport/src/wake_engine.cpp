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

} // namespace transport
