#pragma once

// RAII C++17 wrapper around the XMOS XVF3800 voice-processor I2C control
// interface on the Seeed ReSpeaker carrier. Each Xvf3800 instance owns one
// i2c_master_dev_handle_t and releases it in its destructor.
//
// Wire format for every control write (per the Seeed I2C example):
//     [I2C addr 0x2C][resource_id][command_id][payload_len N][payload...]
//
// All LED commands use resource_id = 20 (GPO servicer).
//
// NOTE: this class is the Phase A proof-of-concept demonstrating the C++
// toolchain integration. It coexists with the legacy C xvf3800.c during
// Phases A-C; Phase C swaps callers over and removes the .c file.

#include <cstdint>
#include <optional>

#include "driver/i2c_master.h"
#include "esp_err.h"

namespace hal {

// XVF3800 LED effect IDs (per the reSpeaker_XVF3800_USB_4MIC_ARRAY
// host_control README — the USB-mic firmware shares the LED control
// servicer with the I2S carrier we use).
enum class Effect : uint8_t {
    Off     = 0,
    Breath  = 1,
    Rainbow = 2,
    Solid   = 3,    // steady single-colour fill
    Doa     = 4,    // direction-of-arrival rotating segment
};

struct Rgb {
    uint8_t r;
    uint8_t g;
    uint8_t b;
};

class Xvf3800 {
public:
    // Factory: probe the chip at the standard 0x2C address, return nullopt
    // on I2C bus-add failure. No throwing constructors — keeps ESP-IDF's
    // -fno-exceptions policy honest.
    static std::optional<Xvf3800> create(i2c_master_bus_handle_t bus);

    // Move-only ownership of the underlying i2c device handle.
    Xvf3800(const Xvf3800&)            = delete;
    Xvf3800& operator=(const Xvf3800&) = delete;
    Xvf3800(Xvf3800&& other) noexcept;
    Xvf3800& operator=(Xvf3800&& other) noexcept;
    ~Xvf3800();

    esp_err_t setEffect(Effect e);
    esp_err_t setBrightness(uint8_t b);
    esp_err_t setSpeed(uint8_t s);
    esp_err_t setColor(Rgb c);

private:
    explicit Xvf3800(i2c_master_dev_handle_t dev) noexcept : dev_(dev) {}

    // Sends [resid][cmd][n][payload...] over I2C. Used by the public
    // setXxx() methods + available to callers that want raw access.
    esp_err_t write(uint8_t resid, uint8_t cmd,
                    const uint8_t* payload, uint8_t n);

    i2c_master_dev_handle_t dev_{nullptr};
};

} // namespace hal
