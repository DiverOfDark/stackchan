#include <catch2/catch_test_macros.hpp>

#include "domain/wake_window.hpp"

using namespace domain;
using namespace std::chrono_literals;

TEST_CASE("does not fire before window fills", "[wake_window]") {
    WakeWindow w({.threshold = 0.10f, .cooldown = 2s});
    bool fired = false;
    for (int i = 0; i < kWakeWindowLen - 1; ++i) {
        if (w.push(0.5f, std::chrono::milliseconds{i})) fired = true;
    }
    REQUIRE_FALSE(fired);
}

TEST_CASE("fires on the sample that crosses threshold", "[wake_window]") {
    WakeWindow w({.threshold = 0.10f, .cooldown = 2s});
    for (int i = 0; i < kWakeWindowLen - 1; ++i) {
        REQUIRE_FALSE(w.push(0.20f, std::chrono::milliseconds{i}));
    }
    REQUIRE(w.push(0.20f, std::chrono::milliseconds{100}));
}

TEST_CASE("doesn't fire when average stays below threshold", "[wake_window]") {
    WakeWindow w({.threshold = 0.50f, .cooldown = 2s});
    bool fired = false;
    for (int i = 0; i < kWakeWindowLen * 3; ++i) {
        if (w.push(0.40f, std::chrono::milliseconds{i * 10})) fired = true;
    }
    REQUIRE_FALSE(fired);
}

TEST_CASE("suppresses re-fire inside cooldown", "[wake_window]") {
    WakeWindow w({.threshold = 0.10f, .cooldown = 2s});
    for (int i = 0; i < kWakeWindowLen; ++i) {
        w.push(0.50f, std::chrono::milliseconds{i});
    }
    // First fire happens above.
    // Window is damped to zero after fire; refill with high probs but
    // still inside the 2 s cooldown.
    bool fired = false;
    for (int i = 0; i < kWakeWindowLen; ++i) {
        if (w.push(0.50f, std::chrono::milliseconds{500 + i})) fired = true;
    }
    REQUIRE_FALSE(fired);
}

TEST_CASE("fires again after cooldown elapses", "[wake_window]") {
    WakeWindow w({.threshold = 0.10f, .cooldown = 2s});
    for (int i = 0; i < kWakeWindowLen; ++i) {
        w.push(0.50f, std::chrono::milliseconds{i});
    }
    // Far past the 2 s cooldown:
    bool fired = false;
    for (int i = 0; i < kWakeWindowLen; ++i) {
        if (w.push(0.50f, std::chrono::milliseconds{5'000 + i})) fired = true;
    }
    REQUIRE(fired);
}

TEST_CASE("clamps out-of-range probabilities", "[wake_window]") {
    WakeWindow w({.threshold = 0.10f, .cooldown = 2s});
    w.push(-0.5f, 0ms);   // should be treated as 0
    REQUIRE(w.lastProbability() == 0.0f);
    w.push(1.5f, 1ms);    // should be treated as 1
    REQUIRE(w.lastProbability() == 1.0f);
}

TEST_CASE("reset clears state", "[wake_window]") {
    WakeWindow w({.threshold = 0.10f, .cooldown = 2s});
    for (int i = 0; i < kWakeWindowLen; ++i) {
        w.push(0.50f, std::chrono::milliseconds{i});
    }
    w.reset();
    REQUIRE(w.lastProbability() == 0.0f);
    // No fire while we re-fill from scratch, even though we're past
    // the original last_fire time.
    for (int i = 0; i < kWakeWindowLen - 1; ++i) {
        REQUIRE_FALSE(w.push(0.50f, std::chrono::milliseconds{10'000 + i}));
    }
}
