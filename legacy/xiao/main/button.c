#include "button.h"

#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "iot_button.h"
#include "button_gpio.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "button";

#define LONG_PRESS_MS  5000

static volatile bool   s_muted = false;
static button_handle_t s_btn   = NULL;

static void on_short_press(void *arg, void *usr_data)
{
    s_muted = !s_muted;
    ESP_LOGI(TAG, "short press → muted=%d", s_muted);
}

static void on_long_press(void *arg, void *usr_data)
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

esp_err_t button_init(void)
{
    button_config_t btn_cfg = {0};
    button_gpio_config_t gpio_cfg = {
        .gpio_num     = BUTTON_MUTE_GPIO,
        .active_level = 0,           // active-low
    };
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &s_btn);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "iot_button_new_gpio_device failed: %s", esp_err_to_name(err));
        return err;
    }

    iot_button_register_cb(s_btn, BUTTON_SINGLE_CLICK, NULL,
                           on_short_press, NULL);

    // Per-callback long-press threshold — beats the global default.
    button_event_args_t long_args = { .long_press = { .press_time = LONG_PRESS_MS } };
    iot_button_register_cb(s_btn, BUTTON_LONG_PRESS_START, &long_args,
                           on_long_press, NULL);
    return ESP_OK;
}

bool button_is_muted(void)
{
    return s_muted;
}
