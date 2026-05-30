#pragma once

// LED ring driver. Owns the Xvf3800 reference, holds the current
// state for dedup, maps domain::LedState → (colour, effect, speed)
// commands. Replaces the old leds.c per-state switch.

#include <mutex>

#include "domain/led_fsm.hpp"
#include "hal/xvf3800.hpp"

namespace app {

class Ui {
public:
    explicit Ui(hal::Xvf3800& ring);

    // Force a specific state (e.g. CONNECTING during boot, WAKE_ACK on
    // wake fire). Honours a brief "hold" window so a WAKE_ACK flash
    // doesn't get immediately masked by the next live-state tick.
    void setLed(domain::LedState state);

    // Live-conversation tick — call from a UI task at ~50 Hz with
    // current session input timestamps; dedups internally.
    void tick(const domain::LedInputs& inputs);

    // --- LED test override (driven by the web LED-test UI) ----------------
    // While an override is active (until hold_ms elapses), setLed()/tick()
    // from the running session are suppressed so the ring shows exactly what
    // these set — otherwise the conversation state machine would revert it
    // within a tick. Auto-resumes when the hold expires. Thread-safe.
    void overrideEffect(hal::Effect e, hal::Rgb colour,
                        uint8_t brightness, uint8_t speed, int hold_ms);
    void overrideState(domain::LedState state, int hold_ms);
    void resumeAuto();   // drop the override now, resume the state machine

private:
    void applyState(domain::LedState s, int64_t now_us);  // raw mapping, no gates
    bool overrideActive(int64_t now_us);                  // resumes on expiry

    hal::Xvf3800&     ring_;
    std::mutex        mu_;
    domain::LedState  last_pushed_   = domain::LedState::Off;
    int64_t           hold_until_us_ = 0;
    int64_t           override_until_us_ = 0;
    bool              force_         = false;   // re-apply once after an override
};

} // namespace app
