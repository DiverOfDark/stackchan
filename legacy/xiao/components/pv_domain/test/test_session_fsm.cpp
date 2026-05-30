#include <catch2/catch_test_macros.hpp>

#include "domain/session_fsm.hpp"

using namespace domain;

TEST_CASE("starts in Idle", "[session_fsm]") {
    SessionFsm s;
    REQUIRE(s.state() == SessionState::Idle);
}

TEST_CASE("happy path: Idle → Connecting → Negotiating → Live", "[session_fsm]") {
    SessionFsm s;
    s.onEvent(SessionEvent::WifiUp);
    REQUIRE(s.state() == SessionState::Connecting);
    s.onEvent(SessionEvent::SignalingStarted);
    REQUIRE(s.state() == SessionState::Negotiating);
    s.onEvent(SessionEvent::PeerLive);
    REQUIRE(s.state() == SessionState::Live);
}

TEST_CASE("PeerLost from Live → Reconnecting", "[session_fsm]") {
    SessionFsm s;
    s.onEvent(SessionEvent::WifiUp);
    s.onEvent(SessionEvent::SignalingStarted);
    s.onEvent(SessionEvent::PeerLive);
    s.onEvent(SessionEvent::PeerLost);
    REQUIRE(s.state() == SessionState::Reconnecting);
}

TEST_CASE("PeerLost from Negotiating → Reconnecting", "[session_fsm]") {
    SessionFsm s;
    s.onEvent(SessionEvent::WifiUp);
    s.onEvent(SessionEvent::SignalingStarted);
    s.onEvent(SessionEvent::PeerLost);
    REQUIRE(s.state() == SessionState::Reconnecting);
}

TEST_CASE("PeerLost from Idle is a no-op", "[session_fsm]") {
    SessionFsm s;
    s.onEvent(SessionEvent::PeerLost);
    REQUIRE(s.state() == SessionState::Idle);
}

TEST_CASE("RetryTick: Reconnecting → Connecting", "[session_fsm]") {
    SessionFsm s;
    s.onEvent(SessionEvent::WifiUp);
    s.onEvent(SessionEvent::SignalingStarted);
    s.onEvent(SessionEvent::PeerLive);
    s.onEvent(SessionEvent::PeerLost);
    s.onEvent(SessionEvent::RetryTick);
    REQUIRE(s.state() == SessionState::Connecting);
}

TEST_CASE("WifiDown from any state → Idle", "[session_fsm]") {
    SessionFsm s;

    // From Connecting:
    s.onEvent(SessionEvent::WifiUp);
    s.onEvent(SessionEvent::WifiDown);
    REQUIRE(s.state() == SessionState::Idle);

    // From Negotiating:
    s.onEvent(SessionEvent::WifiUp);
    s.onEvent(SessionEvent::SignalingStarted);
    s.onEvent(SessionEvent::WifiDown);
    REQUIRE(s.state() == SessionState::Idle);

    // From Live:
    s.onEvent(SessionEvent::WifiUp);
    s.onEvent(SessionEvent::SignalingStarted);
    s.onEvent(SessionEvent::PeerLive);
    s.onEvent(SessionEvent::WifiDown);
    REQUIRE(s.state() == SessionState::Idle);

    // From Reconnecting:
    s.onEvent(SessionEvent::WifiUp);
    s.onEvent(SessionEvent::SignalingStarted);
    s.onEvent(SessionEvent::PeerLive);
    s.onEvent(SessionEvent::PeerLost);
    s.onEvent(SessionEvent::WifiDown);
    REQUIRE(s.state() == SessionState::Idle);
}

TEST_CASE("unexpected event is a no-op", "[session_fsm]") {
    SessionFsm s;
    // From Idle, SignalingStarted shouldn't be possible — must stay Idle.
    s.onEvent(SessionEvent::SignalingStarted);
    REQUIRE(s.state() == SessionState::Idle);
    // From Connecting, PeerLive shouldn't fire — must stay Connecting.
    s.onEvent(SessionEvent::WifiUp);
    s.onEvent(SessionEvent::PeerLive);
    REQUIRE(s.state() == SessionState::Connecting);
}
