#pragma once
// One voice session's WebSocket to the backend (/ws/femto, see
// backend/femto_ws.py): binary frames are 16 kHz PCM16 audio both ways,
// text frames JSON events. Thin RAII wrapper over esp_websocket_client.
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

#include "esp_event.h"
#include "esp_websocket_client.h"

namespace femto {

class WsLink {
public:
    /// Called from the client's task with each chunk of downlink PCM (whole
    /// samples; a sample split across chunks is carried over).
    using AudioFn = std::function<void(const int16_t *pcm, std::size_t samples)>;
    /// Called once per complete text frame.
    using TextFn = std::function<void(std::string json)>;

    WsLink(AudioFn on_audio, TextFn on_text);
    ~WsLink();
    WsLink(const WsLink &) = delete;
    WsLink &operator=(const WsLink &) = delete;

    /// Connect to `url` (ws:// or wss://). Non-blocking; poll open()/dead().
    bool start(const std::string &url);
    /// Socket up (the hello still has to be sent by the caller).
    bool open() const { return open_.load(); }
    /// Disconnected or failed after start(): tear down and reconnect.
    bool dead() const { return dead_.load(); }
    bool sendText(const std::string &text, int timeout_ms);
    bool sendBinary(const void *data, std::size_t len, int timeout_ms);

private:
    static void onEvent(void *arg, esp_event_base_t base, int32_t id, void *data);
    void onData(const esp_websocket_event_data_t &d);

    AudioFn on_audio_;
    TextFn on_text_;
    esp_websocket_client_handle_t client_ = nullptr;
    std::atomic<bool> open_{false};
    std::atomic<bool> dead_{false};
    uint8_t frame_op_ = 0;      // opcode of the frame being received
    std::string text_;          // text frame being reassembled
    uint8_t carry_ = 0;         // odd trailing byte of the last audio chunk
    bool has_carry_ = false;
    int16_t pcm_[1024];         // aligned staging for the audio callback
};

/// https://host/... → wss://host/ws/femto (http → ws).
std::string ws_url_for(const std::string &backend_url);

}  // namespace femto
