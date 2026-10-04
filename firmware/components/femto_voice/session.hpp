#pragma once
// Voice session for Femto. Adapted from pipecat-voice-assistant's
// app::Session (pv_app/session.cpp @ f9fd4e7): same on-demand connect,
// backlog flush, echo guard, reconnect and turn-timeout logic, with the
// XVF3800 LED ring / button / wake-sample upload replaced by CoreS3 audio
// and a state + level output for the Rust UI. The transport is a WebSocket
// to the backend's /ws/femto (raw 16 kHz PCM, JSON events) instead of
// WebRTC, so no TURN relay is involved.
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "audio_cores3.hpp"
#include "domain/session_fsm.hpp"
#include "femto_voice.h"
#include "ws_link.hpp"

namespace femto {

/// Byte FIFO in PSRAM that drops the oldest data when full. Thread-safe.
class Backlog {
public:
    bool init(std::size_t capacity);
    void push(const void *data, std::size_t len);
    std::size_t pop(void *out, std::size_t max);
    void clear();

private:
    std::mutex mtx_;
    uint8_t *buf_ = nullptr;
    std::size_t cap_ = 0, head_ = 0, count_ = 0;
};

class Session {
public:
    Session(std::string backend_url, AudioCores3 &audio);
    void start();
    femto_voice_state_t state() const { return state_.load(); }
    float level() const { return level_.load(); }
    float micLevel() const { return mic_level_.load(); }
    void wake() { ptt_ = true; }
    void setRequestData(std::string json) { hello_ = std::move(json); }
    /** Pop one backend event (JSON); false when none. */
    bool nextEvent(std::string &out);

private:
    bool connect();
    void disconnect();
    std::shared_ptr<WsLink> link();
    static void mainLoopTaskEntry(void *arg);
    static void captureTaskEntry(void *arg);
    static void playbackTaskEntry(void *arg);
    static void senderTaskEntry(void *arg);
    void mainLoopTask();
    void captureTask();
    void playbackTask();
    void senderTask();
    void onLinkUp();
    void onInboundAudio(const int16_t *pcm, std::size_t samples);
    void onText(std::string json);
    void updateState();

    std::string backend_url_;
    std::string hello_ = "{}";
    AudioCores3 &audio_;
    domain::SessionFsm fsm_;
    std::mutex link_mtx_;
    std::shared_ptr<WsLink> link_;
    Backlog uplink_;
    StreamBufferHandle_t playback_buf_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> link_dead_{false};
    std::atomic<bool> conversation_active_{false};
    std::atomic<bool> bot_replied_{false};
    std::atomic<bool> ptt_{false};
    std::atomic<int> chirp_pending_{-1};
    std::atomic<TickType_t> turn_deadline_{0};
    std::atomic<TickType_t> last_rx_frame_tick_{0};
    std::atomic<TickType_t> last_rx_pkt_tick_{0};
    std::atomic<TickType_t> last_mic_active_tick_{0};
    std::atomic<femto_voice_state_t> state_{FEMTO_VOICE_IDLE};
    std::atomic<float> level_{0};
    std::atomic<float> mic_level_{0};
    std::mutex events_mtx_;
    std::deque<std::string> events_;
    std::atomic<uint32_t> rx_audio_chunks_{0};
};

}  // namespace femto
