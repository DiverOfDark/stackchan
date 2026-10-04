#pragma once

// Full-duplex I2S audio bridge to the XVF3800 carrier on the XIAO
// ESP32-S3. The XVF3800 v1.0.7 slave firmware leaves clock generation
// to us; the ESP32-S3 owns BCK + WS and runs the I2S peripheral in
// master mode. 16 kHz, stereo, 32-bit per sample (Philips standard).
//
// We expose RX (mic capture) and TX (speaker playback) as methods on
// one class because the underlying IDF call (i2s_new_channel) issues
// both endpoints in a single transaction — splitting into Mic/Speaker
// classes would force more state synchronisation than it would buy
// in clarity.

#include <cstddef>
#include <cstdint>
#include <optional>

#include "esp_err.h"
#include "driver/i2s_std.h"

namespace hal {

class AudioIo {
public:
    static constexpr int kSampleRate     = 16'000;
    static constexpr int kChannels       = 2;
    static constexpr int kBitsPerSample  = 32;

    // Factory: configure I2S0 in full-duplex master mode on the carrier-
    // board pin map. Returns std::nullopt on driver init failure.
    static std::optional<AudioIo> create();

    AudioIo(const AudioIo&)            = delete;
    AudioIo& operator=(const AudioIo&) = delete;
    AudioIo(AudioIo&& other) noexcept;
    AudioIo& operator=(AudioIo&& other) noexcept;
    ~AudioIo();

    // Blocking read of `frames` stereo frames into `dst` (must hold
    // frames * kChannels int32_t). ESP_OK only if every byte landed.
    esp_err_t read(int32_t* dst, std::size_t frames);

    // Blocking write of `frames` stereo frames from `src`.
    esp_err_t write(const int32_t* src, std::size_t frames);

private:
    AudioIo(i2s_chan_handle_t tx, i2s_chan_handle_t rx) noexcept
        : tx_(tx), rx_(rx) {}

    i2s_chan_handle_t tx_{nullptr};
    i2s_chan_handle_t rx_{nullptr};
};

} // namespace hal
