#include "audio_cores3.hpp"

#include "driver/i2s_std.h"
#include "driver/i2s_tdm.h"
#include "esp_codec_dev.h"
#include "esp_codec_dev_defaults.h"
#include "esp_check.h"
#include "esp_log.h"

namespace femto {
namespace {
constexpr const char *kTag = "audio";
constexpr gpio_num_t kMclk = GPIO_NUM_0;
constexpr gpio_num_t kBclk = GPIO_NUM_34;
constexpr gpio_num_t kWs = GPIO_NUM_33;
constexpr gpio_num_t kDout = GPIO_NUM_13;
constexpr gpio_num_t kDin = GPIO_NUM_14;
constexpr uint8_t kAw88298Addr = 0x36 << 1;  // esp_codec_dev wants 8-bit addresses
constexpr uint8_t kEs7210Addr = 0x40 << 1;
constexpr float kMicGainDb = 30.0f;
}  // namespace

esp_err_t AudioCores3::init(int i2c_port, int volume)
{
    i2s_chan_handle_t tx = nullptr, rx = nullptr;
    i2s_chan_config_t chan = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan.dma_desc_num = 6;
    chan.dma_frame_num = 240;
    chan.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan, &tx, &rx), kTag, "i2s channels");

    i2s_std_config_t std_cfg = {};
    std_cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(kSampleRate);
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    std_cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    std_cfg.gpio_cfg = {.mclk = kMclk, .bclk = kBclk, .ws = kWs, .dout = kDout, .din = I2S_GPIO_UNUSED, .invert_flags = {}};
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(tx, &std_cfg), kTag, "i2s tx");

    i2s_tdm_config_t tdm = {};
    tdm.clk_cfg = I2S_TDM_CLK_DEFAULT_CONFIG(kSampleRate);
    tdm.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
    tdm.clk_cfg.bclk_div = 8;
    tdm.slot_cfg = I2S_TDM_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO,
                                                       i2s_tdm_slot_mask_t(I2S_TDM_SLOT0 | I2S_TDM_SLOT1 | I2S_TDM_SLOT2 | I2S_TDM_SLOT3));
    tdm.gpio_cfg = {.mclk = kMclk, .bclk = kBclk, .ws = kWs, .dout = I2S_GPIO_UNUSED, .din = kDin, .invert_flags = {}};
    ESP_RETURN_ON_ERROR(i2s_channel_init_tdm_mode(rx, &tdm), kTag, "i2s rx");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(tx), kTag, "tx enable");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(rx), kTag, "rx enable");

    audio_codec_i2s_cfg_t i2s_if = {.port = I2S_NUM_0, .rx_handle = rx, .tx_handle = tx};
    const audio_codec_data_if_t *data_if = audio_codec_new_i2s_data(&i2s_if);

    // Legacy I2C port (CONFIG_CODEC_I2C_BACKWARD_COMPATIBLE): shared with the
    // Rust drivers, which own the same port.
    audio_codec_i2c_cfg_t i2c = {.port = (uint8_t)i2c_port, .addr = kAw88298Addr, .bus_handle = nullptr};
    const audio_codec_ctrl_if_t *out_ctrl = audio_codec_new_i2c_ctrl(&i2c);
    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    aw88298_codec_cfg_t aw = {};
    aw.ctrl_if = out_ctrl;
    aw.gpio_if = gpio_if;
    aw.reset_pin = GPIO_NUM_NC;
    aw.hw_gain.pa_voltage = 5.0;
    aw.hw_gain.codec_dac_voltage = 3.3;
    aw.hw_gain.pa_gain = 1;
    const audio_codec_if_t *out_codec = aw88298_codec_new(&aw);
    esp_codec_dev_cfg_t out_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_OUT, .codec_if = out_codec, .data_if = data_if};
    out_dev_ = esp_codec_dev_new(&out_cfg);

    i2c.addr = kEs7210Addr;
    const audio_codec_ctrl_if_t *in_ctrl = audio_codec_new_i2c_ctrl(&i2c);
    es7210_codec_cfg_t es = {};
    es.ctrl_if = in_ctrl;
    es.mic_selected = ES7210_SEL_MIC1 | ES7210_SEL_MIC2 | ES7210_SEL_MIC3;
    const audio_codec_if_t *in_codec = es7210_codec_new(&es);
    esp_codec_dev_cfg_t in_cfg = {.dev_type = ESP_CODEC_DEV_TYPE_IN, .codec_if = in_codec, .data_if = data_if};
    in_dev_ = esp_codec_dev_new(&in_cfg);
    if (!out_dev_ || !in_dev_) {
        ESP_LOGE(kTag, "codec devices");
        return ESP_FAIL;
    }

    esp_codec_dev_sample_info_t in_fs = {};
    in_fs.bits_per_sample = 16;
    in_fs.channel = 2;
    in_fs.channel_mask = ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0);
    in_fs.sample_rate = kSampleRate;
    ESP_RETURN_ON_ERROR(esp_codec_dev_open((esp_codec_dev_handle_t)in_dev_, &in_fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL, kTag, "open mic");
    esp_codec_dev_set_in_channel_gain((esp_codec_dev_handle_t)in_dev_, ESP_CODEC_DEV_MAKE_CHANNEL_MASK(0), kMicGainDb);

    esp_codec_dev_sample_info_t out_fs = {};
    out_fs.bits_per_sample = 16;
    out_fs.channel = 1;
    out_fs.sample_rate = kSampleRate;
    ESP_RETURN_ON_ERROR(esp_codec_dev_open((esp_codec_dev_handle_t)out_dev_, &out_fs) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL, kTag, "open speaker");
    setVolume(volume);
    ESP_LOGI(kTag, "CoreS3 audio up: 16 kHz, mic %.0f dB, volume %d", kMicGainDb, volume);
    return ESP_OK;
}

esp_err_t AudioCores3::read(int16_t *dst, std::size_t n)
{
    return esp_codec_dev_read((esp_codec_dev_handle_t)in_dev_, dst, n * sizeof(int16_t)) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

esp_err_t AudioCores3::write(const int16_t *src, std::size_t n)
{
    return esp_codec_dev_write((esp_codec_dev_handle_t)out_dev_, (void *)src, n * sizeof(int16_t)) == ESP_CODEC_DEV_OK ? ESP_OK : ESP_FAIL;
}

void AudioCores3::setVolume(int volume)
{
    if (out_dev_) {
        esp_codec_dev_set_out_vol((esp_codec_dev_handle_t)out_dev_, volume);
    }
}

}  // namespace femto
