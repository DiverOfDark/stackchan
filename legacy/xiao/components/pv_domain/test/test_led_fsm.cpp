#include <catch2/catch_test_macros.hpp>

#include "domain/led_fsm.hpp"

using namespace domain;
using namespace std::chrono_literals;

namespace {

// "Idle" here means in an active conversation with nothing happening this
// instant — the in-conversation default is Listening.
constexpr LedInputs kIdle{
    .now                 = 10s,
    .last_inbound_audio  = 0ms,    // never
    .last_mic_active     = 0ms,    // never
    .connected           = true,
    .muted               = false,
    .conversation_active = true,
};

} // namespace

TEST_CASE("returns nullopt when !connected", "[led_fsm]") {
    LedInputs in = kIdle;
    in.connected = false;
    REQUIRE_FALSE(resolveLedState(in).has_value());
}

TEST_CASE("Muted beats every other live state", "[led_fsm]") {
    LedInputs in = kIdle;
    in.muted = true;
    in.last_inbound_audio = in.now;   // bot also speaking
    in.last_mic_active    = in.now;   // user also talking
    REQUIRE(*resolveLedState(in) == LedState::Muted);
}

TEST_CASE("Listening when in a conversation with nothing happening", "[led_fsm]") {
    REQUIRE(*resolveLedState(kIdle) == LedState::Listening);
}

TEST_CASE("Off when connected but no conversation is active", "[led_fsm]") {
    LedInputs in = kIdle;
    in.conversation_active = false;
    REQUIRE(*resolveLedState(in) == LedState::Off);
}

TEST_CASE("ambient mic noise does not light Talking before a conversation", "[led_fsm]") {
    LedInputs in = kIdle;
    in.conversation_active = false;
    in.last_mic_active = in.now;   // noise crossed the gate
    REQUIRE(*resolveLedState(in) == LedState::Off);
}

TEST_CASE("Speaking still shows outside a conversation (e.g. greeting)", "[led_fsm]") {
    LedInputs in = kIdle;
    in.conversation_active = false;
    in.last_inbound_audio = in.now;   // bot TTS playing
    REQUIRE(*resolveLedState(in) == LedState::Speaking);
}

TEST_CASE("Speaking while inbound audio is within SPEAKING_HOLD", "[led_fsm]") {
    LedInputs in = kIdle;
    in.last_inbound_audio = in.now - (SPEAKING_HOLD - 1ms);
    REQUIRE(*resolveLedState(in) == LedState::Speaking);

    in.last_inbound_audio = in.now - (SPEAKING_HOLD + 1ms);
    REQUIRE(*resolveLedState(in) != LedState::Speaking);
}

TEST_CASE("Talking while mic active and bot silent", "[led_fsm]") {
    LedInputs in = kIdle;
    in.last_mic_active = in.now - (TALKING_HOLD - 1ms);
    REQUIRE(*resolveLedState(in) == LedState::Talking);

    in.last_mic_active = in.now - (TALKING_HOLD + 1ms);
    REQUIRE(*resolveLedState(in) != LedState::Talking);
}

TEST_CASE("Talking suppressed while Speaking (echo defence)", "[led_fsm]") {
    LedInputs in = kIdle;
    in.last_inbound_audio = in.now;   // bot speaking now
    in.last_mic_active    = in.now;   // AEC residual also crossed gate
    REQUIRE(*resolveLedState(in) == LedState::Speaking);
}

TEST_CASE("Thinking after Talking releases, before TTS arrives", "[led_fsm]") {
    LedInputs in = kIdle;
    // mic dropped TALKING_HOLD+1 ago, so talking=false; we're inside
    // the THINKING_MAX window from there.
    in.last_mic_active = in.now - (TALKING_HOLD + 1ms);
    REQUIRE(*resolveLedState(in) == LedState::Thinking);
}

TEST_CASE("Listening after THINKING window expires with no reply", "[led_fsm]") {
    LedInputs in = kIdle;
    in.last_mic_active = in.now - (TALKING_HOLD + THINKING_MAX + 1ms);
    REQUIRE(*resolveLedState(in) == LedState::Listening);
}

TEST_CASE("Speaking pre-empts an in-flight Thinking", "[led_fsm]") {
    LedInputs in = kIdle;
    in.last_mic_active    = in.now - 1s;     // would be Thinking
    in.last_inbound_audio = in.now;          // …but TTS just arrived
    REQUIRE(*resolveLedState(in) == LedState::Speaking);
}
