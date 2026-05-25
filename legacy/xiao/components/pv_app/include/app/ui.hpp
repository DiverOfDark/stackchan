#pragma once

// LED ring driver. Owns the Xvf3800 reference, holds the current
// state for dedup, maps domain::LedState → (colour, effect, speed)
// commands. Replaces the old leds.c per-state switch.

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

private:
    hal::Xvf3800&     ring_;
    domain::LedState  last_pushed_ = domain::LedState::Off;
    int64_t           hold_until_us_ = 0;
};

} // namespace app
