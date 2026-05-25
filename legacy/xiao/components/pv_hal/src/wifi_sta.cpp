#include "hal/wifi_sta.hpp"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

namespace {

constexpr const char* kTag             = "wifi_sta";
constexpr int         kMaxConnectTries = 5;
constexpr int         kConnectTimeoutMs = 30'000;
constexpr EventBits_t kBitConnected = BIT0;
constexpr EventBits_t kBitFailed    = BIT1;

EventGroupHandle_t g_events     = nullptr;
int                g_tries      = 0;
bool               g_stack_inited = false;

void eventHandler(void* /*arg*/, esp_event_base_t base, int32_t event_id,
                  void* /*data*/)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++g_tries < kMaxConnectTries) {
            ESP_LOGW(kTag, "disconnected, retry %d/%d", g_tries, kMaxConnectTries);
            esp_wifi_connect();
        } else {
            ESP_LOGE(kTag, "exceeded connect attempts; giving up");
            if (g_events) xEventGroupSetBits(g_events, kBitFailed);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        // Reset the counter so a future transient disconnect doesn't
        // immediately exhaust the budget.
        g_tries = 0;
        if (g_events) xEventGroupSetBits(g_events, kBitConnected);
    }
}

} // namespace

namespace hal {

esp_err_t WifiSta::initStack()
{
    if (g_stack_inited) return ESP_OK;
    esp_err_t err;
    if ((err = esp_netif_init())              != ESP_OK) return err;
    if ((err = esp_event_loop_create_default()) != ESP_OK) return err;
    g_events = xEventGroupCreate();
    if (!g_events) return ESP_ERR_NO_MEM;
    g_stack_inited = true;
    return ESP_OK;
}

WifiSta::Result WifiSta::connect(std::string_view ssid, std::string_view psk)
{
    if (initStack() != ESP_OK) return Result::InitFailed;

    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) return Result::InitFailed;

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, nullptr);
    esp_event_handler_register(IP_EVENT,   IP_EVENT_STA_GOT_IP, &eventHandler, nullptr);

    wifi_config_t cfg{};
    std::strncpy(reinterpret_cast<char*>(cfg.sta.ssid),
                 std::string(ssid).c_str(), sizeof(cfg.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(cfg.sta.password),
                 std::string(psk).c_str(),  sizeof(cfg.sta.password) - 1);
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;   // accept open APs too

    if (esp_wifi_set_mode(WIFI_MODE_STA)      != ESP_OK) return Result::InitFailed;
    if (esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK) return Result::InitFailed;

    g_tries = 0;
    xEventGroupClearBits(g_events, kBitConnected | kBitFailed);
    if (esp_wifi_start() != ESP_OK) return Result::InitFailed;

    EventBits_t bits = xEventGroupWaitBits(
        g_events, kBitConnected | kBitFailed,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(kConnectTimeoutMs));
    return (bits & kBitConnected) ? Result::Connected : Result::AuthFailed;
}

} // namespace hal
