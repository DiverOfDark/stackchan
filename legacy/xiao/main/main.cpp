// Application entry point.
//
// Reads as a recipe: init silicon, decide STA vs SoftAP, on STA bring
// up Wi-Fi + SNTP + the WebRTC Session, then sit idle while the worker
// tasks own the runtime. ~80 lines of glue; every meaningful unit of
// behaviour lives in app/transport/hal/domain components.

#include <memory>
#include <string>
#include <vector>

#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_psram.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c_master.h"

#include "hal/audio_io.hpp"
#include "hal/button.hpp"
#include "hal/nvs_kv.hpp"
#include "hal/softap_portal.hpp"
#include "hal/sntp.hpp"
#include "hal/wifi_sta.hpp"
#include "hal/mdns.hpp"
#include "hal/xvf3800.hpp"
#include "app/session.hpp"
#include "app/web_server.hpp"
#include "transport/signaling.hpp"

// HTML payloads, embedded as byte blobs by the *_html.c translation units.
extern "C" {
extern const char  captive_portal_index_html[];
extern const size_t captive_portal_index_html_len;
extern const char  led_test_html[];
extern const size_t led_test_html_len;
}

namespace {

constexpr const char* kTag           = "main";
constexpr const char* kNvsNamespace  = "pipecat-cfg";
constexpr const char* kNvsKeySsid    = "wifi_ssid";
constexpr const char* kNvsKeyPsk     = "wifi_psk";
constexpr const char* kNvsKeyBackend = "backend_url";
constexpr const char* kNvsKeyHostname = "hostname";
constexpr const char* kDefaultHostname = "pipecat-voice";

i2c_master_bus_handle_t openI2cBus()
{
    i2c_master_bus_config_t cfg{};
    cfg.clk_source                   = I2C_CLK_SRC_DEFAULT;
    cfg.i2c_port                     = I2C_NUM_0;
    cfg.sda_io_num                   = GPIO_NUM_5;
    cfg.scl_io_num                   = GPIO_NUM_6;
    cfg.glitch_ignore_cnt            = 7;
    cfg.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t bus = nullptr;
    if (i2c_new_master_bus(&cfg, &bus) != ESP_OK) {
        ESP_LOGE(kTag, "i2c_new_master_bus failed");
    }
    return bus;
}

// Provisioning mode: SoftAP portal collects credentials, persists them
// to NVS, reboots. The reboot lands the next boot in the "has creds"
// branch of decideAndRun().
[[noreturn]] void runProvisioning()
{
    ESP_LOGI(kTag, "no saved credentials — entering provisioning mode");
    auto portal = hal::SoftApPortal::start(
        reinterpret_cast<const uint8_t*>(captive_portal_index_html),
        captive_portal_index_html_len);
    if (!portal) {
        ESP_LOGE(kTag, "SoftAP portal failed to start; rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    portal->setOnCredentials([](const hal::WifiCredentials& c) {
        auto nvs = hal::NvsKv::open(kNvsNamespace);
        if (!nvs) { ESP_LOGE(kTag, "nvs open failed"); return; }
        nvs->setStr(kNvsKeySsid,    c.ssid);
        nvs->setStr(kNvsKeyPsk,     c.psk);
        nvs->setStr(kNvsKeyBackend, c.backend_url);
        nvs->commit();
        ESP_LOGI(kTag, "credentials saved; rebooting");
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
    });
    while (true) vTaskDelay(portMAX_DELAY);
}

void decideAndRun()
{
    if (hal::NvsKv::initPartition() != ESP_OK) {
        ESP_LOGE(kTag, "NVS init failed; rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }

    std::string ssid, psk, backend;
    std::string hostname = kDefaultHostname;
    if (auto nvs = hal::NvsKv::open(kNvsNamespace)) {
        auto s = nvs->getStr(kNvsKeySsid);
        auto b = nvs->getStr(kNvsKeyBackend);
        if (s && b && !s->empty() && !b->empty()) {
            ssid    = std::move(*s);
            backend = std::move(*b);
            if (auto p = nvs->getStr(kNvsKeyPsk)) psk = std::move(*p);
        }
        if (auto hn = nvs->getStr(kNvsKeyHostname); hn && !hn->empty())
            hostname = std::move(*hn);
    }
    if (ssid.empty() || backend.empty()) {
        runProvisioning();
        return;     // unreachable
    }

    ESP_LOGI(kTag, "stored credentials found; connecting to \"%s\"", ssid.c_str());
    auto r = hal::WifiSta::connect(ssid, psk);
    if (r != hal::WifiSta::Result::Connected) {
        ESP_LOGW(kTag, "STA connect failed; clearing creds + restart");
        if (auto nvs = hal::NvsKv::open(kNvsNamespace)) {
            nvs->erase(kNvsKeySsid);
            nvs->erase(kNvsKeyPsk);
            nvs->commit();
        }
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    hal::Sntp::syncOrFallback();
    hal::Mdns::start(hostname);

    // Bring up silicon for audio + LED + button.
    auto bus = openI2cBus();
    if (!bus) esp_restart();

    auto ring = hal::Xvf3800::create(bus);
    auto audio = hal::AudioIo::create();
    auto button = hal::Button::create();
    if (!ring || !audio || !button) {
        ESP_LOGE(kTag, "HAL bring-up failed; rebooting");
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_restart();
    }
    button->setOnLongPress([]() {
        ESP_LOGW(kTag, "long-press: wiping Wi-Fi creds + restart");
        if (auto nvs = hal::NvsKv::open(kNvsNamespace)) {
            nvs->erase(kNvsKeySsid);
            nvs->erase(kNvsKeyPsk);
            nvs->commit();
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        esp_restart();
    });

    // Fetch ICE servers from the backend before composing the session,
    // so libpeer gets the TURN config in its initial offer.
    transport::Signaling discovery{backend};
    auto ice_raw = discovery.fetchIceServers();
    std::vector<transport::PeerIceServer> ice;
    ice.reserve(ice_raw.size());
    for (auto& s : ice_raw) {
        ice.push_back({std::move(s.url), std::move(s.username), std::move(s.credential)});
    }
    ESP_LOGI(kTag, "ICE: %u server(s) configured", (unsigned)ice.size());

    // Session is held statically — its libpeer connection stores a
    // pointer to it as user_data; we need a stable address for the
    // whole program lifetime.
    static app::Session session{
        std::move(backend),
        *ring, *audio, *button,
        std::move(ice),
    };
    session.start();

    // Web console — http://<hostname>.local/ — LED test + /diag + live log
    // stream over WebSocket. Held static like the session.
    static auto web = app::WebServer::start(
        session,
        reinterpret_cast<const uint8_t*>(led_test_html),
        led_test_html_len);
    if (!web) ESP_LOGW(kTag, "web console failed to start");

    // We booted far enough to be reachable + OTA-updatable again → confirm this
    // image so the bootloader won't roll it back. (No-op unless this boot is a
    // freshly-OTA'd image in the pending-verify state.)
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK)
        ESP_LOGI(kTag, "OTA image marked valid");

    // The session's worker tasks own the runtime. Stay alive so
    // ring/audio/button stack-locals don't fall off the end of
    // app_main and get destroyed. (They're move-only RAII objects
    // captured by reference into session — we have to outlive it.)
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(10'000));
        ESP_LOGI(kTag, "alive heap_internal=%u psram_free=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    }
}

} // namespace

extern "C" void app_main()
{
    ESP_LOGI(kTag, "pipecat-voice firmware booting");

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(kTag, "chip=%s cores=%d rev=%d.%d",
             CONFIG_IDF_TARGET, chip.cores,
             chip.revision / 100, chip.revision % 100);

    uint32_t flash_bytes = 0;
    if (esp_flash_get_size(nullptr, &flash_bytes) == ESP_OK) {
        ESP_LOGI(kTag, "flash=%lu MB", (unsigned long)(flash_bytes / (1024 * 1024)));
    }
    ESP_LOGI(kTag, "psram=%u KB", (unsigned)(esp_psram_get_size() / 1024));

    decideAndRun();
}
