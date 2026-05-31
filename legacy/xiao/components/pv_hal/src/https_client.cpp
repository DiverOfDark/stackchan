#include "hal/https_client.hpp"

#include <netdb.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <string>

#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace {

constexpr const char* kTag           = "https_client";
constexpr int         kTimeoutMs     = 15'000;

struct RespAccum {
    hal::HttpResponse* resp;
    std::size_t        cap;
    uint32_t           t_start_ms;       // set just before perform()
    uint32_t           t_connected_ms;   // stamped on HTTP_EVENT_ON_CONNECTED
};

esp_err_t eventCb(esp_http_client_event_t* evt)
{
    auto* a = static_cast<RespAccum*>(evt->user_data);
    if (!a) return ESP_OK;
    // Stamp when TCP + TLS are fully up so we can separate the connect phase
    // (DNS already measured/cached below, so this is TCP+TLS) from the
    // request/response phase. Diagnostic for the on-demand connect latency.
    if (evt->event_id == HTTP_EVENT_ON_CONNECTED) {
        a->t_connected_ms = esp_log_timestamp();
        return ESP_OK;
    }
    if (evt->event_id != HTTP_EVENT_ON_DATA) return ESP_OK;
    if (a->resp->body.size() + evt->data_len > a->cap) {
        ESP_LOGE(kTag, "response > %u bytes — refusing", (unsigned)a->cap);
        return ESP_FAIL;
    }
    a->resp->body.append(reinterpret_cast<const char*>(evt->data), evt->data_len);
    return ESP_OK;
}

// Extract the host from "scheme://host[:port]/path" for an explicit DNS timing.
std::string hostFromUrl(const char* url)
{
    std::string u(url ? url : "");
    auto p = u.find("://");
    std::size_t start = (p == std::string::npos) ? 0 : p + 3;
    std::size_t end = u.find_first_of(":/", start);
    return u.substr(start, end == std::string::npos ? std::string::npos : end - start);
}

} // namespace

namespace hal {

esp_err_t HttpsClient::request(const char* url,
                               esp_http_client_method_t method,
                               std::string_view body,
                               const char* content_type,
                               HttpResponse& out)
{
    out.body.clear();
    RespAccum accum{&out, cap_, 0u, 0u};

    // --- DIAGNOSTIC: time DNS resolution explicitly. getaddrinfo populates the
    // lwIP resolver cache, so esp_http_client's own lookup inside perform() is a
    // cache hit and the connect phase we measure below is just TCP+TLS. This
    // tells us whether the on-demand cold-connect cost is DNS vs handshake. ---
    const std::string host = hostFromUrl(url);
    long dns_ms = -1;
    char ipbuf[48] = "?";
    if (!host.empty()) {
        addrinfo hints{};
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* res = nullptr;
        const uint32_t d0 = esp_log_timestamp();
        const int gai = getaddrinfo(host.c_str(), "443", &hints, &res);
        dns_ms = (long)(esp_log_timestamp() - d0);
        if (gai == 0 && res) {
            if (res->ai_family == AF_INET) {
                auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
                inet_ntop(AF_INET, &sin->sin_addr, ipbuf, sizeof ipbuf);
            } else if (res->ai_family == AF_INET6) {
                auto* sin6 = reinterpret_cast<sockaddr_in6*>(res->ai_addr);
                inet_ntop(AF_INET6, &sin6->sin6_addr, ipbuf, sizeof ipbuf);
            }
            freeaddrinfo(res);
        } else {
            snprintf(ipbuf, sizeof ipbuf, "gai_err=%d", gai);
        }
    }

    esp_http_client_config_t cfg{};
    cfg.url               = url;
    cfg.method            = method;
    cfg.event_handler     = eventCb;
    cfg.user_data         = &accum;
    cfg.timeout_ms        = kTimeoutMs;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_ERR_NO_MEM;

    if (content_type) {
        esp_http_client_set_header(cli, "Content-Type", content_type);
    }
    if (!body.empty()) {
        // esp_http_client_set_post_field stores the pointer; body must
        // outlive perform() which is true in this synchronous call.
        esp_http_client_set_post_field(cli, body.data(),
                                       static_cast<int>(body.size()));
    }

    accum.t_start_ms = esp_log_timestamp();
    esp_err_t err = esp_http_client_perform(cli);
    const uint32_t t_end_ms = esp_log_timestamp();
    out.status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);

    const long connect_ms = accum.t_connected_ms
                              ? (long)(accum.t_connected_ms - accum.t_start_ms) : -1;
    const long total_ms   = (long)(t_end_ms - accum.t_start_ms);
    ESP_LOGI(kTag, "timing %s: dns=%ldms(ip=%s) tcp+tls=%ldms total=%ldms status=%d",
             host.c_str(), dns_ms, ipbuf, connect_ms, total_ms, out.status);
    return err;
}

} // namespace hal
