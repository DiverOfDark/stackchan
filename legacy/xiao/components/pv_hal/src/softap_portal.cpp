#include "hal/softap_portal.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

#include "domain/form_urlencoded.hpp"

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/wifi_sta.hpp"   // initStack()

namespace {
constexpr const char* kTag           = "softap";
constexpr int         kMaxBodyBytes  = 512;
constexpr int         kPortDefault   = 80;
constexpr const char* kSsidPrefix    = "pipecat-voice-";
} // namespace

namespace hal {

struct SoftApPortal::Impl {
    httpd_handle_t                  http      = nullptr;
    const uint8_t*                  html      = nullptr;
    std::size_t                     html_len  = 0;
    SoftApPortal::OnCredentialsFn   on_creds;
};

// Singleton pointer so the C-style HTTPD callbacks can find the Impl.
// Only one provisioning portal runs at a time in practice; if that
// ever changes we'd plug it through req->user_ctx.
static SoftApPortal::Impl* g_impl = nullptr;

namespace {

esp_err_t handleRoot(httpd_req_t* req)
{
    if (!g_impl) return ESP_FAIL;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req,
                           reinterpret_cast<const char*>(g_impl->html),
                           g_impl->html_len);
}

esp_err_t handleSave(httpd_req_t* req)
{
    if (!g_impl) return ESP_FAIL;

    int remaining = req->content_len;
    if (remaining < 0 || remaining > kMaxBodyBytes) {
        httpd_resp_set_status(req, "413 Payload Too Large");
        return httpd_resp_sendstr(req, "request body too large");
    }
    std::vector<char> body(remaining + 1);
    int total = 0;
    while (total < remaining) {
        int got = httpd_req_recv(req, body.data() + total, remaining - total);
        if (got == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (got <= 0) {
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "incomplete form submission");
        }
        total += got;
    }
    body[total] = '\0';

    std::string_view sv{body.data(), static_cast<std::size_t>(total)};
    auto ssid    = domain::formGet(sv, "ssid");
    auto backend = domain::formGet(sv, "backend");
    auto psk_opt = domain::formGet(sv, "psk");
    if (!ssid || !backend) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "missing ssid or backend");
    }

    WifiCredentials creds{*ssid, psk_opt.value_or(""), *backend};
    if (g_impl->on_creds) {
        g_impl->on_creds(creds);
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;max-width:24rem;margin:2rem auto'>"
        "<h1>Saved</h1><p>Rebooting now…</p></body></html>");
    return ESP_OK;
}

} // namespace

std::optional<SoftApPortal> SoftApPortal::start(const uint8_t* html, std::size_t html_len,
                                                uint16_t http_port)
{
    if (WifiSta::initStack() != ESP_OK) return std::nullopt;

    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (esp_wifi_init(&init_cfg) != ESP_OK) return std::nullopt;

    uint8_t mac[6]{};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    wifi_config_t cfg{};
    cfg.ap.channel        = 1;
    cfg.ap.max_connection = 4;
    cfg.ap.authmode       = WIFI_AUTH_OPEN;
    std::snprintf(reinterpret_cast<char*>(cfg.ap.ssid), sizeof(cfg.ap.ssid),
                  "%s%02x%02x", kSsidPrefix, mac[4], mac[5]);
    cfg.ap.ssid_len = std::strlen(reinterpret_cast<char*>(cfg.ap.ssid));

    if (esp_wifi_set_mode(WIFI_MODE_AP)        != ESP_OK ||
        esp_wifi_set_config(WIFI_IF_AP, &cfg)  != ESP_OK ||
        esp_wifi_start()                       != ESP_OK) {
        ESP_LOGE(kTag, "softap bring-up failed");
        return std::nullopt;
    }

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port    = http_port == 0 ? kPortDefault : http_port;
    httpd_handle_t http = nullptr;
    if (httpd_start(&http, &http_cfg) != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed");
        return std::nullopt;
    }

    SoftApPortal p;
    p.impl_       = new Impl{};
    p.impl_->http     = http;
    p.impl_->html     = html;
    p.impl_->html_len = html_len;
    g_impl            = p.impl_;

    httpd_uri_t root{ "/",     HTTP_GET,  handleRoot, nullptr };
    httpd_uri_t save{ "/save", HTTP_POST, handleSave, nullptr };
    httpd_register_uri_handler(http, &root);
    httpd_register_uri_handler(http, &save);

    ESP_LOGI(kTag, "SoftAP up: SSID=\"%s\" — open http://192.168.4.1/", cfg.ap.ssid);
    return p;
}

SoftApPortal::SoftApPortal(SoftApPortal&& other) noexcept : impl_(other.impl_)
{
    other.impl_ = nullptr;
    if (impl_) g_impl = impl_;
}

SoftApPortal& SoftApPortal::operator=(SoftApPortal&& other) noexcept
{
    if (this != &other) {
        if (impl_) { if (impl_->http) httpd_stop(impl_->http); delete impl_; }
        impl_ = other.impl_;
        other.impl_ = nullptr;
        if (impl_) g_impl = impl_;
    }
    return *this;
}

SoftApPortal::~SoftApPortal()
{
    if (impl_) {
        if (g_impl == impl_) g_impl = nullptr;
        if (impl_->http) httpd_stop(impl_->http);
        delete impl_;
    }
}

void SoftApPortal::setOnCredentials(OnCredentialsFn fn)
{
    if (impl_) impl_->on_creds = std::move(fn);
}

} // namespace hal
