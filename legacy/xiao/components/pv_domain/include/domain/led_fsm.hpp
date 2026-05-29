#pragma once

// Live-conversation LED state machine. Pure logic — no I/O, no globals,
// no platform headers. Maps the time-stamped session inputs (last
// inbound TTS, last loud mic, mute, connected) to a single LedState.
// The HAL/transport layer is responsible for actually pushing that
// state to the ring; this layer just decides which state is right.
//
// Priority order (highest first):
//   Muted     — hardware switch trumps everything
//   Speaking  — inbound TTS energy in last SPEAKING_HOLD_MS
//   Talking   — local mic energy in last TALKING_HOLD_MS (suppressed
//               while Speaking — AEC residual would falsely trip it)
//   Thinking  — mic just dropped, no TTS yet, within THINKING_MAX_MS
//   Listening — default idle
//
// When !connected the function declines to set anything (returns
// std::nullopt) — connecting / negotiating LEDs are driven by the
// peer-state callback path, not this tick.

#include <chrono>
#include <cstdint>
#include <optional>

namespace domain {

enum class LedState : uint8_t {
    Off,
    Provisioning,    // SoftAP up, waiting for credentials
    Connecting,      // STA-connecting or signaling handshake
    Negotiating,     // WebRTC peer past signaling, ICE/DTLS in progress
    Listening,       // session live, waiting for the user to start
    Talking,         // local-mic energy → user is speaking now
    Thinking,        // user just stopped, backend round-trip in flight
    WakeAck,         // brief flash on wake-word detected
    Speaking,        // TTS playback in progress (inbound audio energy)
    Muted,           // mic muted by button
    Error,
};

// Millisecond timestamps. Domain doesn't care whether they come from
// xTaskGetTickCount or steady_clock — the values just have to be
// monotonic and in the same units within a single call.
using Ms = std::chrono::milliseconds;

struct LedInputs {
    Ms   now;
    Ms   last_inbound_audio;   // bumped when an energetic TTS frame arrives
    Ms   last_mic_active;      // bumped when local mic RMS crosses the gate
    bool connected;            // peer past DTLS-COMPLETED
    bool muted;                // hardware mute switch
};

// Hold-time tuning. SPEAKING bridges natural inter-word silences inside
// a TTS reply (comma in "Привет, Кирилл" is ~300 ms). TALKING bridges
// sub-syllable silences in user speech. THINKING starts when TALKING
// releases and clears when SPEAKING starts or the window times out.
inline constexpr Ms SPEAKING_HOLD{1500};
inline constexpr Ms TALKING_HOLD{600};
inline constexpr Ms THINKING_MAX{5000};

// Returns the LedState the ring should display given `in`, or
// std::nullopt when the live-conversation path declines to set
// anything (i.e. !in.connected).
std::optional<LedState> resolveLedState(const LedInputs& in);

} // namespace domain
