#pragma once

// Device web console (STA mode), served on port 80. One page that can:
//   * preview every LED state / drive any raw XVF3800 effect (Ui override),
//   * report internal state as JSON (/diag),
//   * change the mDNS hostname at runtime (/hostname),
//   * stream the firmware log over a WebSocket (/ws/logs).
//
// The log stream is the important bit: this board's only ESP32 console is the
// USB-Serial-JTAG, and opening it from a host resets the chip — so the serial
// log is unusable for "it breaks after a while" debugging. Mirroring the log
// over a WebSocket makes it readable (and forwardable) over the network with
// zero resets. Mirrors hal::SoftApPortal's esp_http_server pattern; one
// instance for the program lifetime.

#include <cstddef>
#include <cstdint>
#include <optional>

#include "esp_http_server.h"

#include "app/session.hpp"

namespace app {

class WebServer {
public:
    // Drives/inspects `session` (must outlive the server — the static Session).
    // `html`/`html_len` is the embedded console page.
    static std::optional<WebServer> start(Session& session, const uint8_t* html,
                                          std::size_t html_len, uint16_t port = 80);

    WebServer(const WebServer&)            = delete;
    WebServer& operator=(const WebServer&) = delete;
    WebServer(WebServer&& o) noexcept : http_(o.http_) { o.http_ = nullptr; }
    WebServer& operator=(WebServer&& o) noexcept
    {
        if (this != &o) { stop(); http_ = o.http_; o.http_ = nullptr; }
        return *this;
    }
    ~WebServer() { stop(); }

private:
    explicit WebServer(httpd_handle_t h) noexcept : http_(h) {}
    void stop() noexcept { if (http_) { httpd_stop(http_); http_ = nullptr; } }

    httpd_handle_t http_ = nullptr;
};

} // namespace app
