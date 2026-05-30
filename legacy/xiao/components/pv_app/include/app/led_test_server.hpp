#pragma once

// Tiny HTTP server (STA mode) that serves an LED-test web page and drives the
// ring through Ui's override path. Lets you preview every LedState mapping and
// every raw XVF3800 effect/colour/brightness/speed from a browser, to decide
// which effect should map to which state. Mirrors hal::SoftApPortal's
// esp_http_server pattern. One instance for the program lifetime.

#include <cstddef>
#include <cstdint>
#include <optional>

#include "esp_http_server.h"

#include "app/ui.hpp"

namespace app {

class LedTestServer {
public:
    // Start on `port` (default 80 — the SoftAP portal is gone by STA time).
    // `html`/`html_len` is the embedded LED-test page. Drives `ui` (must
    // outlive the server — in practice the static Session's Ui).
    static std::optional<LedTestServer> start(Ui& ui, const uint8_t* html,
                                              std::size_t html_len, uint16_t port = 80);

    LedTestServer(const LedTestServer&)            = delete;
    LedTestServer& operator=(const LedTestServer&) = delete;
    LedTestServer(LedTestServer&& o) noexcept : http_(o.http_) { o.http_ = nullptr; }
    LedTestServer& operator=(LedTestServer&& o) noexcept
    {
        if (this != &o) { stop(); http_ = o.http_; o.http_ = nullptr; }
        return *this;
    }
    ~LedTestServer() { stop(); }

private:
    explicit LedTestServer(httpd_handle_t h) noexcept : http_(h) {}
    void stop() noexcept { if (http_) { httpd_stop(http_); http_ = nullptr; } }

    httpd_handle_t http_ = nullptr;
};

} // namespace app
