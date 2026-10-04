#pragma once

// High-level session lifecycle FSM. Replaces the implicit state spread
// across webrtc_session.c's {connected, conv_active, retry_at_tick}
// booleans with one explicit enum + one transition function.
//
//   Idle ──WifiUp──▶ Connecting ──SignalingStarted──▶ Negotiating
//                                                        │
//                                                        ▼
//   Idle ◀──WifiDown──── any ─────────PeerLost───▶ Reconnecting
//                                                        │
//                                  RetryTick             │
//                            Connecting ◀────────────────┘
//
//   Negotiating ──PeerLive──▶ Live ──PeerLost──▶ Reconnecting
//
//   Live ──WifiDown──▶ Idle
//
// Each event from each state is exhaustively handled; an unexpected
// event is a no-op (with the previous state preserved). Pure logic;
// no platform / RTOS deps. Host-testable.

#include <cstdint>

namespace domain {

enum class SessionState : uint8_t {
    Idle,           // no network, or pre-connect
    Connecting,     // Wi-Fi up; about to POST /api/offer
    Negotiating,    // POST sent; ICE/DTLS in progress
    Live,           // DTLS-COMPLETED; audio flowing
    Reconnecting,   // peer dropped; retry timer running
};

enum class SessionEvent : uint8_t {
    WifiUp,
    WifiDown,
    SignalingStarted,   // POST /api/offer just issued
    PeerLive,           // PEER_CONNECTION_COMPLETED from libpeer
    PeerLost,           // FAILED / DISCONNECTED / CLOSED
    RetryTick,          // retry timer fired in Reconnecting
};

class SessionFsm {
public:
    SessionState state() const { return state_; }
    void onEvent(SessionEvent ev);

private:
    SessionState state_ = SessionState::Idle;
};

} // namespace domain
