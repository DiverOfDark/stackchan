#include "domain/led_fsm.hpp"

namespace domain {

std::optional<LedState> resolveLedState(const LedInputs& in)
{
    if (!in.connected) {
        return std::nullopt;
    }
    if (in.muted) {
        return LedState::Muted;
    }

    // Treat zero timestamps as far-past so the function is well-defined
    // on a cold start where neither tick has been bumped yet.
    const Ms since_rx  = in.last_inbound_audio.count() == 0
                           ? Ms::max() : in.now - in.last_inbound_audio;
    const Ms since_mic = in.last_mic_active.count() == 0
                           ? Ms::max() : in.now - in.last_mic_active;

    const bool speaking = since_rx < SPEAKING_HOLD;
    if (speaking) {
        return LedState::Speaking;
    }

    // No active conversation → ring stays dark. This is what keeps the LED
    // off in idle (instead of a permanent "Listening" glow) and stops ambient
    // mic noise from lighting Talking before the wake word arms a turn.
    if (!in.conversation_active) {
        return LedState::Off;
    }

    // TALKING is suppressed while bot is speaking — AEC residual would
    // trip the mic gate during the bot's reply otherwise. This branch
    // is unreachable in that case (speaking was true), but the
    // dependency is explicit in the !speaking guard so the rule reads
    // the same in source and in tests.
    const bool talking = since_mic < TALKING_HOLD;
    if (talking) {
        return LedState::Talking;
    }

    const bool thinking = since_mic < TALKING_HOLD + THINKING_MAX;
    if (thinking) {
        return LedState::Thinking;
    }

    return LedState::Listening;
}

} // namespace domain
