#include "audio_io.h"

#include <string.h>

#include "driver/gpio.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "audio_io";

#define I2S_BCK_PIN    GPIO_NUM_8
#define I2S_WS_PIN     GPIO_NUM_7
#define I2S_DOUT_PIN   GPIO_NUM_44
#define I2S_DIN_PIN    GPIO_NUM_43

#define LOOPBACK_FRAMES 256   // ~16 ms @ 16 kHz; small enough for low latency

static i2s_chan_handle_t s_tx_chan = NULL;
static i2s_chan_handle_t s_rx_chan = NULL;
static bool              s_loopback_started = false;

esp_err_t audio_io_init(void)
{
    if (s_tx_chan != NULL && s_rx_chan != NULL) {
        return ESP_OK;
    }

    // The XVF3800 v1.0.7 I2S firmware is the *slave* variant (the master
    // firmware is 48 kHz-only and forces a resample everywhere). With the
    // slave fw loaded, the ESP32-S3 has to generate BCK + WS itself.
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    // dma_desc_num x dma_frame_num governs latency vs DMA underrun headroom.
    // 6 descriptors x 240 frames @ 16 kHz = ~90 ms of buffering before drop.
    chan_cfg.dma_desc_num  = 6;
    chan_cfg.dma_frame_num = 240;
    chan_cfg.auto_clear    = true;

    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, &s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_IO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(
            I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = I2S_BCK_PIN,
            .ws   = I2S_WS_PIN,
            .dout = I2S_DOUT_PIN,
            .din  = I2S_DIN_PIN,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv   = false,
            },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX init failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_init_std_mode(s_rx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "TX enable failed: %s", esp_err_to_name(err));
        return err;
    }
    err = i2s_channel_enable(s_rx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "RX enable failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "I2S slave ready: %d Hz, %d ch, %d bit (BCK=%d WS=%d DOUT=%d DIN=%d)",
             AUDIO_IO_SAMPLE_RATE, AUDIO_IO_CHANNELS, AUDIO_IO_BITS_PER_SAMPLE,
             I2S_BCK_PIN, I2S_WS_PIN, I2S_DOUT_PIN, I2S_DIN_PIN);
    return ESP_OK;
}

esp_err_t audio_io_read(int32_t *dst, size_t frames)
{
    if (s_rx_chan == NULL) return ESP_ERR_INVALID_STATE;
    const size_t want = frames * AUDIO_IO_CHANNELS * sizeof(int32_t);
    size_t got = 0;
    esp_err_t err = i2s_channel_read(s_rx_chan, dst, want, &got, portMAX_DELAY);
    if (err != ESP_OK) return err;
    return (got == want) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

esp_err_t audio_io_write(const int32_t *src, size_t frames)
{
    if (s_tx_chan == NULL) return ESP_ERR_INVALID_STATE;
    const size_t want = frames * AUDIO_IO_CHANNELS * sizeof(int32_t);
    size_t wrote = 0;
    esp_err_t err = i2s_channel_write(s_tx_chan, src, want, &wrote, portMAX_DELAY);
    if (err != ESP_OK) return err;
    return (wrote == want) ? ESP_OK : ESP_ERR_INVALID_SIZE;
}

static void loopback_task(void *arg)
{
    static int32_t buf[LOOPBACK_FRAMES * AUDIO_IO_CHANNELS];
    uint32_t iter = 0;
    while (1) {
        esp_err_t err = audio_io_read(buf, LOOPBACK_FRAMES);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "read err: %s", esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        err = audio_io_write(buf, LOOPBACK_FRAMES);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "write err: %s", esp_err_to_name(err));
        }
        if ((++iter % 200) == 0) {
            // Once every ~3.2 s, dump a sample so we know audio is flowing.
            ESP_LOGI(TAG, "loopback alive iter=%lu sample[0]=%ld sample[1]=%ld",
                     (unsigned long)iter, (long)buf[0], (long)buf[1]);
        }
    }
}

esp_err_t audio_io_start_loopback(void)
{
    if (s_loopback_started) return ESP_OK;
    BaseType_t ok = xTaskCreatePinnedToCore(
        loopback_task, "audio_loopback", 4096, NULL, 8, NULL, 1);
    if (ok != pdPASS) {
        ESP_LOGE(TAG, "failed to spawn loopback task");
        return ESP_FAIL;
    }
    s_loopback_started = true;
    ESP_LOGI(TAG, "loopback task started");
    return ESP_OK;
}
