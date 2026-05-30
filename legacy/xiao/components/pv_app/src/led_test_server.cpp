#include "app/led_test_server.hpp"

#include <cstdlib>

#include "esp_log.h"

namespace {

constexpr const char* kTag = "led_test";

// Override hold per command. Each click re-arms it; after this much idle the
// ring returns to the live conversation state machine on its own.
constexpr int kHoldMs = 60'000;

// Handlers are C function pointers, so context goes through a file-local
// singleton (same pattern as hal::SoftApPortal). The server lives for the
// whole program, so there's no teardown race.
struct Ctx {
    app::Ui*       ui       = nullptr;
    const uint8_t* html     = nullptr;
    std::size_t    html_len = 0;
};
Ctx g_ctx{};

int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

// Read one integer query parameter; returns `def` if absent/unparseable.
int query_int(httpd_req_t* req, const char* key, int def)
{
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return def;
    char v[16];
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return def;
    return atoi(v);
}

esp_err_t handleRoot(httpd_req_t* req)
{
    if (!g_ctx.html) return ESP_FAIL;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, reinterpret_cast<const char*>(g_ctx.html),
                           g_ctx.html_len);
}

// /effect?e=<0-4>&r=&g=&b=&br=&sp=  — raw XVF3800 effect override.
esp_err_t handleEffect(httpd_req_t* req)
{
    if (!g_ctx.ui) return ESP_FAIL;
    const int e  = clampi(query_int(req, "e",  3),   0, 4);
    const int r  = clampi(query_int(req, "r",  0),   0, 255);
    const int g  = clampi(query_int(req, "g",  0),   0, 255);
    const int b  = clampi(query_int(req, "b",  0),   0, 255);
    const int br = clampi(query_int(req, "br", 0x60), 0, 255);
    const int sp = clampi(query_int(req, "sp", 0x40), 0, 255);
    g_ctx.ui->overrideEffect(
        static_cast<hal::Effect>(e),
        hal::Rgb{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)},
        static_cast<uint8_t>(br), static_cast<uint8_t>(sp), kHoldMs);
    return httpd_resp_sendstr(req, "ok");
}

// /state?s=<0-10>  — preview the current mapping for a LedState.
esp_err_t handleState(httpd_req_t* req)
{
    if (!g_ctx.ui) return ESP_FAIL;
    const int s = clampi(query_int(req, "s", 0), 0, 10);
    g_ctx.ui->overrideState(static_cast<domain::LedState>(s), kHoldMs);
    return httpd_resp_sendstr(req, "ok");
}

// /resume  — drop the override, hand the ring back to the state machine.
esp_err_t handleResume(httpd_req_t* req)
{
    if (!g_ctx.ui) return ESP_FAIL;
    g_ctx.ui->resumeAuto();
    return httpd_resp_sendstr(req, "ok");
}

} // namespace

namespace app {

std::optional<LedTestServer> LedTestServer::start(Ui& ui, const uint8_t* html,
                                                  std::size_t html_len, uint16_t port)
{
    g_ctx = Ctx{&ui, html, html_len};

    httpd_config_t cfg   = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = port;
    cfg.lru_purge_enable = true;   // don't wedge if a browser leaves sockets open

    httpd_handle_t h = nullptr;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed on port %u", (unsigned)port);
        return std::nullopt;
    }

    httpd_uri_t u_root  { "/",       HTTP_GET, handleRoot,   nullptr };
    httpd_uri_t u_eff   { "/effect", HTTP_GET, handleEffect, nullptr };
    httpd_uri_t u_state { "/state",  HTTP_GET, handleState,  nullptr };
    httpd_uri_t u_res   { "/resume", HTTP_GET, handleResume, nullptr };
    httpd_register_uri_handler(h, &u_root);
    httpd_register_uri_handler(h, &u_eff);
    httpd_register_uri_handler(h, &u_state);
    httpd_register_uri_handler(h, &u_res);

    ESP_LOGI(kTag, "LED-test web UI up on http://<device-ip>:%u/", (unsigned)port);
    return LedTestServer{h};
}

} // namespace app
