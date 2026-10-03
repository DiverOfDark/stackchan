#pragma once
// Voice session for Femto. Adapted from pipecat-voice-assistant's
// app::Session (pv_app/session.cpp @ f9fd4e7): same on-demand connect,
// backlog flush, echo guard, reconnect and turn-timeout logic, with the
// XVF3800 LED ring / button / wake-sample upload replaced by CoreS3 audio
// and a state + level output for the Rust UI.
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "audio_cores3.hpp"
#include "domain/g722.hpp"
#include "domain/session_fsm.hpp"
#include "femto_voice.h"
#include "transport/peer.hpp"
#include "transport/signaling.hpp"

namespace femto {

class Session {
public:
    Session(std::string backend_url, AudioCores3 &audio);
    void start();
    femto_voice_state_t state() const { return state_.load(); }
    float level() const { return level_.load(); }
    float micLevel() const { return mic_level_.load(); }
    void wake() { ptt_ = true; }
    void setRequestData(std::string json) { signaling_.setRequestData(std::move(json)); }
    /** Pop one backend event (JSON); false when none. */
    bool nextEvent(std::string &out);

private:
    bool buildAndOffer();
    static void mainLoopTaskEntry(void *arg);
    static void captureTaskEntry(void *arg);
    static void playbackTaskEntry(void *arg);
    void mainLoopTask();
    void captureTask();
    void playbackTask();
    void onPeerState(transport::PeerState s);
    void onLocalSdp(std::string sdp);
    void onInboundAudio(const uint8_t *data, std::size_t size);
    void updateState();

    std::string backend_url_;
    AudioCores3 &audio_;
    std::vector<transport::PeerIceServer> ice_;
    domain::SessionFsm fsm_;
    transport::Signaling signaling_;
    domain::G722Codec g722_enc_;
    domain::G722Codec g722_dec_;
    std::mutex peer_mtx_;
    std::unique_ptr<transport::Peer> peer_;
    StreamBufferHandle_t playback_buf_ = nullptr;

    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<bool> peer_dead_{false};
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
    // Diagnostics kept from the original Session.
    std::atomic<int> last_peer_state_{-1};
    std::atomic<uint32_t> reconnects_{0};
    std::atomic<uint32_t> rx_audio_pkts_{0};
    std::atomic<int32_t> rx_audio_last_peak_{0};
    std::atomic<int32_t> rx_audio_max_peak_{0};
};

}  // namespace femto
