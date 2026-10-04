#include "ws_link.hpp"

#include <algorithm>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace femto {
namespace {
constexpr const char *kTag = "voice_ws";
constexpr uint8_t kOpText = 0x1;
constexpr uint8_t kOpBinary = 0x2;
constexpr uint8_t kOpContinuation = 0x0;
constexpr std::size_t kMaxTextFrame = 4096;
}  // namespace

std::string ws_url_for(const std::string &backend_url)
{
    std::string url = backend_url;
    while (!url.empty() && url.back() == '/') url.pop_back();
    if (url.rfind("https://", 0) == 0) url = "wss://" + url.substr(8);
    else if (url.rfind("http://", 0) == 0) url = "ws://" + url.substr(7);
    return url + "/ws/femto";
}

WsLink::WsLink(AudioFn on_audio, TextFn on_text) : on_audio_(std::move(on_audio)), on_text_(std::move(on_text)) {}

WsLink::~WsLink()
{
    if (!client_) return;
    // Clean close if we can; destroy stops the task either way.
    if (esp_websocket_client_is_connected(client_)) esp_websocket_client_close(client_, pdMS_TO_TICKS(500));
    esp_websocket_client_destroy(client_);
}

bool WsLink::start(const std::string &url)
{
    esp_websocket_client_config_t cfg = {};
    cfg.uri = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    // One session per connection: a drop is handled by the Session (it
    // reconnects with a fresh hello), never silently by the client.
    cfg.disable_auto_reconnect = true;
    cfg.task_name = "voice_ws";
    cfg.task_prio = 8;
    cfg.task_stack = 6 * 1024;  // internal RAM (the 1.8 client has no PSRAM stack option)
    cfg.buffer_size = 2048;
    cfg.network_timeout_ms = 5000;
    cfg.ping_interval_sec = 5;
    cfg.pingpong_timeout_sec = 15;
    cfg.user_context = this;
    client_ = esp_websocket_client_init(&cfg);
    if (!client_) {
        ESP_LOGE(kTag, "init failed");
        return false;
    }
    esp_websocket_register_events(client_, WEBSOCKET_EVENT_ANY, &WsLink::onEvent, this);
    if (esp_websocket_client_start(client_) != ESP_OK) {
        ESP_LOGE(kTag, "start failed");
        return false;
    }
    return true;
}

bool WsLink::sendText(const std::string &text, int timeout_ms)
{
    if (!client_ || !open_.load()) return false;
    return esp_websocket_client_send_text(client_, text.data(), static_cast<int>(text.size()), pdMS_TO_TICKS(timeout_ms)) ==
           static_cast<int>(text.size());
}

bool WsLink::sendBinary(const void *data, std::size_t len, int timeout_ms)
{
    if (!client_ || !open_.load()) return false;
    return esp_websocket_client_send_bin(client_, static_cast<const char *>(data), static_cast<int>(len), pdMS_TO_TICKS(timeout_ms)) ==
           static_cast<int>(len);
}

void WsLink::onEvent(void *arg, esp_event_base_t, int32_t id, void *data)
{
    auto *self = static_cast<WsLink *>(arg);
    switch (id) {
    case WEBSOCKET_EVENT_CONNECTED:
        ESP_LOGI(kTag, "connected");
        self->open_ = true;
        break;
    case WEBSOCKET_EVENT_DATA:
        self->onData(*static_cast<esp_websocket_event_data_t *>(data));
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
    case WEBSOCKET_EVENT_ERROR: {
        const auto *d = static_cast<esp_websocket_event_data_t *>(data);
        if (!self->dead_.exchange(true))
            ESP_LOGW(kTag, "link down (event %ld, error type %d, tls 0x%x, errno %d, close %d)", static_cast<long>(id),
                     d ? d->error_handle.error_type : -1, d ? d->error_handle.esp_tls_last_esp_err : 0,
                     d ? d->error_handle.esp_transport_sock_errno : 0, d ? d->close_status_code : 0);
        self->open_ = false;
        break;
    }
    default:
        break;
    }
}

void WsLink::onData(const esp_websocket_event_data_t &d)
{
    // Frames bigger than the client buffer arrive in pieces (payload_offset);
    // continuation frames keep the opcode of the frame they continue.
    if (d.op_code == kOpText || d.op_code == kOpBinary) {
        if (d.payload_offset == 0) frame_op_ = d.op_code;
    } else if (d.op_code != kOpContinuation) {
        return;  // ping/pong/close: handled by the client
    }
    if (d.data_len <= 0) return;

    if (frame_op_ == kOpBinary) {
        const auto *p = reinterpret_cast<const uint8_t *>(d.data_ptr);
        std::size_t n = static_cast<std::size_t>(d.data_len);
        while (n > 0) {
            // Rebuild whole little-endian samples, carrying an odd byte over.
            auto *out = reinterpret_cast<uint8_t *>(pcm_);
            std::size_t o = 0;
            if (has_carry_) {
                out[o++] = carry_;
                has_carry_ = false;
            }
            const std::size_t take = std::min(n, sizeof(pcm_) - o);
            std::memcpy(out + o, p, take);
            o += take;
            p += take;
            n -= take;
            if (o % 2) {
                carry_ = out[--o];
                has_carry_ = true;
            }
            if (o) on_audio_(pcm_, o / 2);
        }
    } else if (frame_op_ == kOpText) {
        if (d.payload_offset == 0) text_.clear();
        if (text_.size() + d.data_len <= kMaxTextFrame) text_.append(d.data_ptr, d.data_len);
        if (d.payload_offset + d.data_len >= d.payload_len) {
            if (d.fin) {
                on_text_(std::move(text_));
                text_.clear();
            }
        }
    }
}

}  // namespace femto
