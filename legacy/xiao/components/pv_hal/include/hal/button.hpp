#pragma once

// Mute-switch button on the XIAO ESP32-S3 BOOT GPIO. Wraps
// espressif/iot-button so the rest of the firmware can observe mute
// state without dragging in iot_button.h. Long-press semantics
// (factory-reset) are owned by the caller via setOnLongPress() — the
// HAL doesn't reach for NVS / esp_restart on its own.

#include <cstdint>
#include <functional>
#include <optional>

#include "esp_err.h"

namespace hal {

struct ButtonInternal;     // friend of Button; lives in button.cpp

class Button {
    friend struct ButtonInternal;

public:
    // GPIO defaults to BOOT on the XIAO ESP32-S3 (GPIO 0). Active-low.
    static std::optional<Button> create(int gpio = 0);

    Button(const Button&)            = delete;
    Button& operator=(const Button&) = delete;
    Button(Button&& other) noexcept;
    Button& operator=(Button&& other) noexcept;
    ~Button();

    // Current mute state, latched on each SINGLE_CLICK.
    bool isMuted() const { return muted_; }

    // Optional long-press hook — invoked from the iot_button task
    // when the user holds the button > 5 s. App layer wires it to
    // "wipe Wi-Fi creds + esp_restart".
    using LongPressFn = std::function<void()>;
    void setOnLongPress(LongPressFn fn);

private:
    explicit Button(void* handle) noexcept : handle_(handle) {}

    // iot_button handle (typed as void* here to keep iot_button.h out
    // of this header — caller code doesn't need it).
    void*       handle_     = nullptr;
    bool        muted_      = false;
    LongPressFn long_press_;
};

} // namespace hal
