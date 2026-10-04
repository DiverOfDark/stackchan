#pragma once

// Wi-Fi STA bring-up. Blocking connect() with a retry-bounded join
// against the configured SSID/PSK; returns true once we have an IP.
// On a clean exit (success) the underlying esp_netif + esp_wifi handles
// stay live for the lifetime of the program — Wi-Fi is process-wide
// in ESP-IDF, so a per-instance teardown would be a fiction.
//
// The wifi_provision.c that this replaces also did NVS reads, SoftAP
// fallback, and SNTP sync inline. We split those into NvsKv,
// SoftApPortal, and Sntp respectively.

#include <string>
#include <string_view>

#include "esp_err.h"

namespace hal {

class WifiSta {
public:
    enum class Result {
        Connected,         // got an IP
        AuthFailed,        // retries exhausted (likely wrong PSK / out of range)
        InitFailed,        // esp_wifi_init / netif / events setup failed
    };

    // One-time global netif + event loop init. Idempotent. Required
    // before any WifiSta::connect() or SoftApPortal::start().
    static esp_err_t initStack();

    // Blocking connect — joins ssid/psk and waits up to ~30 s for an
    // IPv4 address. Up to 5 internal retry attempts.
    static Result connect(std::string_view ssid, std::string_view psk);
};

} // namespace hal
