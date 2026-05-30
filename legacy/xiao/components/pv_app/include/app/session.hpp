#pragma once

// Application-level session: composes Peer + Signaling + Capture +
// Playback + Ui and runs the FreeRTOS tasks that drive the voice
// loop. Replaces the bulk of webrtc_session.c.
//
// Lifetime: one Session instance lives for the whole "we have Wi-Fi"
// period of the program. Internally it can rebuild the libpeer
// connection on transient drops (retry timer).

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "domain/audio_frame.hpp"
#include "domain/session_fsm.hpp"
#include "hal/audio_io.hpp"
#include "hal/button.hpp"
#include "hal/xvf3800.hpp"
#include "transport/peer.hpp"
#include "transport/signaling.hpp"
#include "app/ui.hpp"

namespace app {

class Session {
public:
    Session(std::string                       backend_url,
            hal::Xvf3800&                     ring,
            hal::AudioIo&                     audio,
            hal::Button&                      button,
            std::vector<transport::PeerIceServer> ice);

    Session(const Session&)            = delete;
    Session& operator=(const Session&) = delete;
    Session(Session&&)                 = delete;
    Session& operator=(Session&&)      = delete;
    ~Session();

    // Spawn the worker tasks + start the libpeer connection. Returns
    // immediately; the conversation runs in the background until
    // stop() is called.
    void start();
    void stop();

    // The LED ring driver, exposed so the web LED-test UI can drive effects.
    Ui& ui() { return ui_; }

    // Snapshot of internal state as a JSON object, for the /diag web endpoint.
    // Lets us inspect a misbehaving device over the network (no serial / reset).
    std::string diagJson();

private:
    // Build a fresh Peer + push the local offer. Called once at
    // start() and again on retry-tick after a PeerLost.
    bool buildAndOffer();

    // Static thunks for the FreeRTOS task entries.
    static void mainLoopTaskEntry (void* arg);
    static void captureTaskEntry  (void* arg);
    static void playbackTaskEntry (void* arg);

    void mainLoopTask();
    void captureTask();
    void playbackTask();

    // libpeer callbacks (run on libpeer's internal thread).
    void onPeerState(transport::PeerState s);
    void onLocalSdp(std::string sdp);
    void onInboundAudio(const uint8_t* data, std::size_t size);

    std::string                            backend_url_;
    hal::Xvf3800&                          ring_;
    hal::AudioIo&                          audio_;
    hal::Button&                           button_;
    std::vector<transport::PeerIceServer>  ice_;

    Ui                                     ui_;
    domain::SessionFsm                     fsm_;
    transport::Signaling                   signaling_;
    // Audio on the wire is G.711 µ-law @ 8 kHz (see domain/g711.hpp) — encode
    // and decode are stateless, so no codec objects to hold here.
    std::unique_ptr<transport::Peer>        peer_;       // rebuilt per session

    // Jitter buffer for inbound TTS (filled by onInboundAudio, drained
    // by playback task).
    StreamBufferHandle_t                   playback_buf_ = nullptr;

    TaskHandle_t                           t_main_       = nullptr;
    TaskHandle_t                           t_cap_        = nullptr;
    TaskHandle_t                           t_play_       = nullptr;

    std::atomic<bool>                      running_{false};
    std::atomic<bool>                      connected_{false};
    std::atomic<int>                       last_peer_state_{-1};   // transport::PeerState, -1 = none
    std::atomic<uint32_t>                  reconnects_{0};         // PeerLost count
    std::atomic<uint32_t>                  rx_audio_pkts_{0};      // inbound (downlink) audio packets
    std::atomic<int32_t>                   rx_audio_last_peak_{0}; // peak of the last decoded frame
    std::atomic<int32_t>                   rx_audio_max_peak_{0};  // loudest decoded frame since boot
    // Conversation gate: false until the wake word fires, true while a turn is
    // in progress, back to false after a silence timeout. Mic audio is only
    // streamed to the backend while this is true.
    std::atomic<bool>                      conversation_active_{false};
    // Tick by which the turn ends unless pushed forward by user speech (await
    // regime) or a bot TTS frame (post-reply regime). See the capture task.
    std::atomic<TickType_t>                turn_deadline_{0};
    std::atomic<TickType_t>                last_rx_frame_tick_{0};
    std::atomic<TickType_t>                last_mic_active_tick_{0};
    std::atomic<TickType_t>                retry_at_tick_{0};
};

} // namespace app
