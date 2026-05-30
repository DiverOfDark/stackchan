#include "hal/mdns.hpp"

#include "esp_log.h"
#include "mdns.h"

namespace {
constexpr const char* kTag = "mdns";
}

namespace hal {

bool Mdns::start(const std::string& hostname)
{
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(kTag, "mdns_init failed");
        return false;
    }
    mdns_hostname_set(hostname.c_str());
    mdns_instance_name_set("pipecat-voice");
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    ESP_LOGI(kTag, "mDNS up: http://%s.local/", hostname.c_str());
    return true;
}

void Mdns::setHostname(const std::string& hostname)
{
    mdns_hostname_set(hostname.c_str());
    ESP_LOGI(kTag, "mDNS hostname → %s.local", hostname.c_str());
}

} // namespace hal
