#include "domain/session_fsm.hpp"

namespace domain {

void SessionFsm::onEvent(SessionEvent ev)
{
    // WifiDown returns to Idle from any state — handle once at the top.
    if (ev == SessionEvent::WifiDown) {
        state_ = SessionState::Idle;
        return;
    }
    // PeerLost goes to Reconnecting from any "session up" state.
    if (ev == SessionEvent::PeerLost) {
        if (state_ == SessionState::Negotiating ||
            state_ == SessionState::Live ||
            state_ == SessionState::Reconnecting) {
            state_ = SessionState::Reconnecting;
        }
        return;
    }

    switch (state_) {
    case SessionState::Idle:
        if (ev == SessionEvent::WifiUp) state_ = SessionState::Connecting;
        break;

    case SessionState::Connecting:
        if (ev == SessionEvent::SignalingStarted) state_ = SessionState::Negotiating;
        break;

    case SessionState::Negotiating:
        if (ev == SessionEvent::PeerLive) state_ = SessionState::Live;
        break;

    case SessionState::Live:
        // Stays Live. PeerLost / WifiDown handled above.
        break;

    case SessionState::Reconnecting:
        if (ev == SessionEvent::RetryTick) state_ = SessionState::Connecting;
        break;
    }
}

} // namespace domain
