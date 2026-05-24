#include "ota.h"

#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "ota";

esp_err_t ota_check_and_update(const char *url)
{
    if (!url || !*url) return ESP_ERR_INVALID_ARG;
    ESP_LOGI(TAG, "starting OTA from %s", url);

    esp_http_client_config_t http_cfg = {
        .url               = url,
        .timeout_ms        = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_cfg = {
        .http_config = &http_cfg,
    };

    esp_err_t err = esp_https_ota(&ota_cfg);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "OTA succeeded, rebooting into new image");
        esp_restart();
    } else {
        ESP_LOGE(TAG, "OTA failed: %s", esp_err_to_name(err));
    }
    return err;
}
