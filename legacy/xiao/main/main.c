#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_io.h"
#include "webrtc_session.h"
#include "wifi_provision.h"

static const char *TAG = "main";

void app_main(void)
{
    ESP_LOGI(TAG, "pipecat-voice firmware booting");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "chip=%s cores=%d rev=%d.%d",
             CONFIG_IDF_TARGET, chip.cores,
             chip.revision / 100, chip.revision % 100);

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(NULL, &flash_bytes) == ESP_OK) {
        ESP_LOGI(TAG, "flash=%lu MB", (unsigned long)(flash_bytes / (1024 * 1024)));
    }

    size_t psram_bytes = esp_psram_get_size();
    ESP_LOGI(TAG, "psram=%u KB", (unsigned)(psram_bytes / 1024));

    ESP_ERROR_CHECK(audio_io_init());
    ESP_ERROR_CHECK(wifi_provision_start());

    if (wifi_provision_state() == WIFI_PROV_STATE_CONNECTED) {
        const char *backend = wifi_provision_backend_url();
        ESP_LOGI(TAG, "wifi connected, backend=%s", backend ? backend : "(unset)");
        if (backend) {
            ESP_ERROR_CHECK(webrtc_session_start(backend));
        }
    } else {
        // No backend reachable — fall back to local audio loopback so the
        // M1 ear-test still works during provisioning.
        ESP_ERROR_CHECK(audio_io_start_loopback());
    }

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        ESP_LOGI(TAG, "alive heap_internal=%u psram_free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}
