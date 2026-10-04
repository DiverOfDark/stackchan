#include "hal/xvf3800.hpp"

#include <cstring>
#include <utility>

#include "esp_log.h"

// Mirror the I2C constants that the C version (firmware/main/xvf3800.c)
// uses, so the two implementations behave identically until Phase C
// retires the C path.
namespace {

constexpr const char* kTag             = "xvf3800";
constexpr uint8_t     kI2cAddress      = 0x2C;
constexpr uint32_t    kI2cClockHz      = 400'000;     // XVF3800 datasheet
constexpr int         kI2cTimeoutMs    = 100;
constexpr uint8_t     kMaxPayloadBytes = 32;          // generous; LED ops use ≤ 4
constexpr uint8_t     kResidGpo        = 20;          // GPO servicer
constexpr uint8_t     kCmdLedEffect    = 12;
constexpr uint8_t     kCmdLedBrightness = 13;
constexpr uint8_t     kCmdLedSpeed     = 15;
constexpr uint8_t     kCmdLedColor     = 16;

} // namespace

namespace hal {

std::optional<Xvf3800> Xvf3800::create(i2c_master_bus_handle_t bus)
{
    if (bus == nullptr) {
        ESP_LOGE(kTag, "Xvf3800::create called with null bus");
        return std::nullopt;
    }

    // Value-init zeros every byte, then we set the fields we care about.
    // C++ designated initializers require listing every field or zero-init
    // up front; the latter is easier to keep in sync with IDF updates.
    i2c_device_config_t dev_cfg{};
    dev_cfg.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev_cfg.device_address  = kI2cAddress;
    dev_cfg.scl_speed_hz    = kI2cClockHz;
    i2c_master_dev_handle_t dev = nullptr;
    esp_err_t err = i2c_master_bus_add_device(bus, &dev_cfg, &dev);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2c add device 0x%02x failed: %s",
                 kI2cAddress, esp_err_to_name(err));
        return std::nullopt;
    }

    // Quick probe — ack confirms wiring + power. Non-fatal: user may be
    // testing with the carrier disconnected.
    if (i2c_master_probe(bus, kI2cAddress, kI2cTimeoutMs) != ESP_OK) {
        ESP_LOGW(kTag, "no ack at 0x%02x — XVF3800 not responding", kI2cAddress);
    }

    return Xvf3800{dev};
}

Xvf3800::Xvf3800(Xvf3800&& other) noexcept : dev_(other.dev_)
{
    other.dev_ = nullptr;
}

Xvf3800& Xvf3800::operator=(Xvf3800&& other) noexcept
{
    if (this != &other) {
        if (dev_) i2c_master_bus_rm_device(dev_);
        dev_ = other.dev_;
        other.dev_ = nullptr;
    }
    return *this;
}

Xvf3800::~Xvf3800()
{
    if (dev_) {
        // Ignore the return — destructor's only job is "best-effort release".
        i2c_master_bus_rm_device(dev_);
    }
}

esp_err_t Xvf3800::write(uint8_t resid, uint8_t cmd,
                         const uint8_t* payload, uint8_t n)
{
    if (!dev_) return ESP_ERR_INVALID_STATE;
    if (n > kMaxPayloadBytes) return ESP_ERR_INVALID_SIZE;

    uint8_t buf[3 + kMaxPayloadBytes];
    buf[0] = resid;
    buf[1] = cmd;
    buf[2] = n;
    if (n) std::memcpy(&buf[3], payload, n);

    return i2c_master_transmit(dev_, buf, 3 + n, kI2cTimeoutMs);
}

esp_err_t Xvf3800::setEffect(Effect e)
{
    auto v = static_cast<uint8_t>(e);
    return write(kResidGpo, kCmdLedEffect, &v, 1);
}

esp_err_t Xvf3800::setBrightness(uint8_t b)
{
    return write(kResidGpo, kCmdLedBrightness, &b, 1);
}

esp_err_t Xvf3800::setSpeed(uint8_t s)
{
    return write(kResidGpo, kCmdLedSpeed, &s, 1);
}

esp_err_t Xvf3800::setColor(Rgb c)
{
    // Seeed example serialises colour LSB-first into a 4-byte payload as
    // (B, G, R, 0x00) — wiki.seeedstudio.com/respeaker_xvf3800_xiao_rgb/.
    uint8_t payload[4] = { c.b, c.g, c.r, 0x00 };
    return write(kResidGpo, kCmdLedColor, payload, 4);
}

} // namespace hal
