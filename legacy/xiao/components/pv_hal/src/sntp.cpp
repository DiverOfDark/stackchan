#include "hal/sntp.hpp"

#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_log.h"
#include "esp_netif_sntp.h"

namespace {
constexpr const char* kTag = "sntp";
} // namespace

namespace hal {

void Sntp::syncOrFallback(std::chrono::milliseconds timeout)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    cfg.start = true;
    esp_netif_sntp_init(&cfg);

    if (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout.count())) == ESP_OK) {
        time_t now = time(nullptr);
        ESP_LOGI(kTag, "synced: %s", ctime(&now));
        return;
    }

    // Fallback: use the firmware build date. __DATE__ format is
    // "Mmm dd yyyy". Advances on every rebuild so we never drift
    // backwards across versions.
    struct tm bt{};
    char mon[4] = {0};
    if (std::sscanf(__DATE__, "%3s %d %d", mon, &bt.tm_mday, &bt.tm_year) == 3) {
        const char* mons = "JanFebMarAprMayJunJulAugSepOctNovDec";
        const char* p = std::strstr(mons, mon);
        bt.tm_mon = p ? (p - mons) / 3 : 0;
        bt.tm_year -= 1900;
        time_t when = mktime(&bt);
        struct timeval tv{when, 0};
        settimeofday(&tv, nullptr);
        ESP_LOGW(kTag, "sync timed out; using build-date fallback %s", __DATE__);
    }
}

} // namespace hal
