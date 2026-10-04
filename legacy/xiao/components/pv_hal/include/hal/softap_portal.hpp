#pragma once

// SoftAP + HTTP captive-portal for first-time Wi-Fi provisioning.
// Brings up the AP, serves a static index.html with a credentials
// form on GET /, and invokes a caller-supplied callback when the
// form is submitted at POST /save.
//
// The callback decides what to do with the credentials (typically:
// store via NvsKv + esp_restart to land in STA mode on next boot).
// This class doesn't reach for NVS or reboot on its own — keeps
// responsibilities clean.

#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "esp_err.h"

namespace hal {

struct WifiCredentials {
    std::string ssid;
    std::string psk;          // may be empty for open APs
    std::string backend_url;
};

class SoftApPortal {
public:
    using OnCredentialsFn = std::function<void(const WifiCredentials&)>;

    // SSID is "pipecat-voice-XXXX" where XXXX comes from the last 2
    // bytes of the STA MAC.
    //
    // Captive-portal HTML must be supplied as a byte pointer + length
    // — the static blob is embedded into the firmware as a separate
    // translation unit (captive_portal_index_html.c, generated from
    // captive_portal_index_html.html by tools/embed_html.py).
    static std::optional<SoftApPortal> start(const uint8_t* html, std::size_t html_len,
                                             uint16_t http_port = 80);

    SoftApPortal(const SoftApPortal&)            = delete;
    SoftApPortal& operator=(const SoftApPortal&) = delete;
    SoftApPortal(SoftApPortal&& other) noexcept;
    SoftApPortal& operator=(SoftApPortal&& other) noexcept;
    ~SoftApPortal();

    void setOnCredentials(OnCredentialsFn fn);

    struct Impl;     // public so the .cpp's static g_impl can refer to it
                     // without a friend declaration.

private:
    SoftApPortal() = default;
    Impl* impl_ = nullptr;
};

} // namespace hal
