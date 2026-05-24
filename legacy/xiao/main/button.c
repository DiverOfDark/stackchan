#include "button.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "button";

#define POLL_INTERVAL_MS    20
#define DEBOUNCE_MS         40
#define LONG_PRESS_MS       5000

static volatile bool s_muted = false;

static void wipe_wifi_creds_and_reboot(void)
{
    nvs_handle_t h;
    if (nvs_open("pipecat-cfg", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "wifi_ssid");
        nvs_erase_key(h, "wifi_psk");
        // Leave backend_url so re-provisioning only asks for Wi-Fi.
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGW(TAG, "long-press: cleared Wi-Fi creds, rebooting");
    vTaskDelay(pdMS_TO_TICKS(200));
    esp_restart();
}

static void button_task(void *arg)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << BUTTON_MUTE_GPIO),
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    // Active-low button: 0 == pressed.
    int  last_level   = 1;
    int  press_start  = -1;
    TickType_t now = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(POLL_INTERVAL_MS));
        now = xTaskGetTickCount();
        int level = gpio_get_level(BUTTON_MUTE_GPIO);

        if (level == 0 && last_level == 1) {
            press_start = (int)now;
        } else if (level == 1 && last_level == 0 && press_start >= 0) {
            int held_ms = (int)((now - press_start) * portTICK_PERIOD_MS);
            if (held_ms >= DEBOUNCE_MS && held_ms < LONG_PRESS_MS) {
                s_muted = !s_muted;
                ESP_LOGI(TAG, "short press → muted=%d", s_muted);
            }
            press_start = -1;
        } else if (level == 0 && press_start >= 0) {
            int held_ms = (int)((now - press_start) * portTICK_PERIOD_MS);
            if (held_ms >= LONG_PRESS_MS) {
                wipe_wifi_creds_and_reboot();  // does not return
            }
        }
        last_level = level;
    }
}

esp_err_t button_init(void)
{
    BaseType_t ok = xTaskCreate(button_task, "button", 3072, NULL, 5, NULL);
    return ok == pdPASS ? ESP_OK : ESP_FAIL;
}

bool button_is_muted(void)
{
    return s_muted;
}
