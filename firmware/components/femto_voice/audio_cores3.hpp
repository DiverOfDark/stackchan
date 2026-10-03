#pragma once
// CoreS3 audio: I2S0 duplex at 16 kHz — std TX to the AW88298 amp, TDM RX
// from the ES7210 mic ADC — with codec control via esp_codec_dev. Pins and
// codec setup from M5's StackChan firmware (cores3_audio_codec.cc).
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

namespace femto {

class AudioCores3 {
public:
    static constexpr int kSampleRate = 16'000;

    esp_err_t init(int i2c_port, int volume);
    /** Mono int16 mic samples (blocks until `n` are read). */
    esp_err_t read(int16_t *dst, std::size_t n);
    /** Mono int16 speaker samples. */
    esp_err_t write(const int16_t *src, std::size_t n);
    void setVolume(int volume);

private:
    void *in_dev_ = nullptr;
    void *out_dev_ = nullptr;
};

}  // namespace femto
