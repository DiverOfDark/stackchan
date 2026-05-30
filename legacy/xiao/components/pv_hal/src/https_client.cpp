#include "hal/https_client.hpp"

#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace {

constexpr const char* kTag           = "https_client";
constexpr int         kTimeoutMs     = 15'000;

struct RespAccum {
    hal::HttpResponse* resp;
    std::size_t        cap;
};

esp_err_t eventCb(esp_http_client_event_t* evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || !evt->user_data) return ESP_OK;
    auto* a = static_cast<RespAccum*>(evt->user_data);
    if (a->resp->body.size() + evt->data_len > a->cap) {
        ESP_LOGE(kTag, "response > %u bytes — refusing", (unsigned)a->cap);
        return ESP_FAIL;
    }
    a->resp->body.append(reinterpret_cast<const char*>(evt->data), evt->data_len);
    return ESP_OK;
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
    RespAccum accum{&out, cap_};

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

    esp_err_t err = esp_http_client_perform(cli);
    out.status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    return err;
}

} // namespace hal
