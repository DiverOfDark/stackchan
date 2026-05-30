#pragma once

// Live-conversation LED state machine. Pure logic — no I/O, no globals,
// no platform headers. Maps the time-stamped session inputs (last
// inbound TTS, last loud mic, mute, connected) to a single LedState.
// The HAL/transport layer is responsible for actually pushing that
// state to the ring; this layer just decides which state is right.
//
// Priority order (highest first):
//   Muted     — hardware switch trumps everything
//   Speaking  — inbound TTS energy in last SPEAKING_HOLD_MS (shown even
//               outside a conversation, e.g. the connect greeting)
//   Off       — connected but no conversation in progress: ring stays dark
//               so it only lights for an actual interaction (and ambient
//               mic noise can't trip Talking when nobody asked)
//   Talking   — local mic energy in last TALKING_HOLD_MS (suppressed
//               while Speaking — AEC residual would falsely trip it)
//   Thinking  — mic just dropped, no TTS yet, within THINKING_MAX_MS
//   Listening — in-conversation idle, waiting for the user to speak
//
// A "conversation" is armed by the wake word and ends after a silence
// timeout (Session owns that flag and passes it in as conversation_active).
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
    bool conversation_active;  // wake word fired, turn not yet timed out
};

// Hold-time tuning. SPEAKING bridges silences *inside* a TTS reply — not just
// inter-word commas but the longer gaps between sentences — so a multi-sentence
// answer stays one steady pink breath instead of flickering pink↔green.
// TALKING bridges sub-syllable silences in user speech. THINKING starts when
// TALKING releases and holds until SPEAKING starts (or the turn ends): it must
// cover the *whole* backend round-trip, which on the on-demand path is connect
// + buffered-flush + STT + LLM + TTS ≈ 10-15 s. A short THINKING_MAX used to
// expire mid-wait and drop the ring back to green "Listening" while the backend
// was still working — read as "it gave up". Keep it amber for the full wait.
inline constexpr Ms SPEAKING_HOLD{2500};
inline constexpr Ms TALKING_HOLD{1500};
inline constexpr Ms THINKING_MAX{15000};

// Returns the LedState the ring should display given `in`, or
// std::nullopt when the live-conversation path declines to set
// anything (i.e. !in.connected).
std::optional<LedState> resolveLedState(const LedInputs& in);

} // namespace domain
