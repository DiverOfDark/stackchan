#include "hal/audio_io.hpp"

#include <cstring>
#include <utility>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace {

constexpr const char* kTag        = "audio_io";
constexpr gpio_num_t  kPinBclk    = GPIO_NUM_8;
constexpr gpio_num_t  kPinWs      = GPIO_NUM_7;
constexpr gpio_num_t  kPinDout    = GPIO_NUM_44;
constexpr gpio_num_t  kPinDin     = GPIO_NUM_43;

} // namespace

namespace hal {

std::optional<AudioIo> AudioIo::create()
{
    // dma_desc_num x dma_frame_num governs latency vs underrun headroom:
    // 6 × 240 @ 16 kHz ≈ 90 ms buffering before a drop.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear    = true;

    i2s_chan_handle_t tx = nullptr;
    i2s_chan_handle_t rx = nullptr;
    esp_err_t err = i2s_new_channel(&chan_cfg, &tx, &rx);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return std::nullopt;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = kPinBclk,
            .ws   = kPinWs,
            .dout = kPinDout,
            .din  = kPinDin,
            .invert_flags = {false, false, false},
        },
    };

    if ((err = i2s_channel_init_std_mode(tx, &std_cfg)) != ESP_OK ||
        (err = i2s_channel_init_std_mode(rx, &std_cfg)) != ESP_OK ||
        (err = i2s_channel_enable(tx))                  != ESP_OK ||
        (err = i2s_channel_enable(rx))                  != ESP_OK) {
        ESP_LOGE(kTag, "I2S init/enable failed: %s", esp_err_to_name(err));
        i2s_del_channel(tx);
        i2s_del_channel(rx);
        return std::nullopt;
    }

    ESP_LOGI(kTag, "I2S full-duplex master ready: %d Hz, %d ch, %d bit",
             kSampleRate, kChannels, kBitsPerSample);
    return AudioIo{tx, rx};
}

AudioIo::AudioIo(AudioIo&& other) noexcept
    : tx_(other.tx_), rx_(other.rx_)
{
    other.tx_ = nullptr;
    other.rx_ = nullptr;
}

AudioIo& AudioIo::operator=(AudioIo&& other) noexcept
{
    if (this != &other) {
        if (tx_) i2s_del_channel(tx_);
        if (rx_) i2s_del_channel(rx_);
        tx_ = other.tx_;
        rx_ = other.rx_;
        other.tx_ = nullptr;
        other.rx_ = nullptr;
    }
    return *this;
}

AudioIo::~AudioIo()
{
    if (tx_) i2s_del_channel(tx_);
    if (rx_) i2s_del_channel(rx_);
}

esp_err_t AudioIo::read(int32_t* dst, std::size_t frames)
{
    if (!rx_) return ESP_ERR_INVALID_STATE;
    const std::size_t bytes = frames * kChannels * sizeof(int32_t);
    std::size_t got = 0;
    return i2s_channel_read(rx_, dst, bytes, &got, portMAX_DELAY) == ESP_OK
        && got == bytes ? ESP_OK : ESP_FAIL;
}

esp_err_t AudioIo::write(const int32_t* src, std::size_t frames)
{
    if (!tx_) return ESP_ERR_INVALID_STATE;
    const std::size_t bytes = frames * kChannels * sizeof(int32_t);
    std::size_t put = 0;
    return i2s_channel_write(tx_, src, bytes, &put, portMAX_DELAY) == ESP_OK
        && put == bytes ? ESP_OK : ESP_FAIL;
}

} // namespace hal
