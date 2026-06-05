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
#include <mutex>
#include <string>
#include <vector>

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "domain/audio_frame.hpp"
#include "domain/chirp.hpp"
#include "domain/g722.hpp"
#include "domain/session_fsm.hpp"
#include "hal/audio_io.hpp"
#include "hal/button.hpp"
#include "hal/xvf3800.hpp"
#include "transport/peer.hpp"
#include "transport/signaling.hpp"
#include "transport/wake_engine.hpp"
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

    // Copy the most recent wake-trigger sample as a WAV (header + PCM) plus a
    // metadata JSON and its sequence number, under wake_mtx_. Returns false if
    // nothing has been captured since boot. Used by the /wake.wav + /wake.json
    // web endpoints and by the backend uploader task.
    bool getWakeSample(std::string& wav, std::string& meta, uint32_t& seq);
    uint32_t wakeCaptureSeq() const { return wake_capture_seq_.load(); }

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
    // Audio on the wire is G.722 wideband @ 16 kHz (see domain/g722.hpp). Both
    // directions are stateful, so we hold one codec per direction and reset
    // them on every (re)connect in buildAndOffer().
    domain::G722Codec                      g722_enc_;   // uplink (capture task)
    domain::G722Codec                      g722_dec_;   // downlink (onInboundAudio)
    // Guards peer_ lifetime: the capture task sends on peer_ from the AV core
    // while mainLoop builds/tears it down on the main core. Held briefly around
    // sendAudio (capture) and reset (mainLoop) so a teardown can't free peer_
    // mid-send. peer_->tick() (mainLoop, same task as reset) stays lock-free.
    std::mutex                             peer_mtx_;
    std::unique_ptr<transport::Peer>        peer_;       // built per turn (on wake)

    // Jitter buffer for inbound TTS (filled by onInboundAudio, drained
    // by playback task).
    StreamBufferHandle_t                   playback_buf_ = nullptr;

    // --- Wake-trigger audio capture (hard-negative collection) ------------
    // A snapshot of the mic audio (mono_uplink, 16 kHz int16) leading up to the
    // most recent wake fire, plus the decision metrics that fired it. captureTask
    // fills it on each fire from its rolling ring; the web server serves it
    // (/wake.wav, /wake.json) and the uploader pushes it to the backend. The
    // strong false positives are indistinguishable from real wakes at the metric
    // level, so the only way to improve the model is to collect the actual audio.
    std::mutex                             wake_mtx_;
    int16_t*                               wake_pcm_     = nullptr;   // PSRAM snapshot
    std::size_t                            wake_pcm_len_ = 0;         // valid samples
    transport::WakeMetrics                 wake_metrics_ = {};
    std::atomic<uint32_t>                  wake_capture_seq_{0};      // bumped per snapshot

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
    // Tick of the last inbound audio packet (ANY, incl. silence keep-alive).
    // A healthy backend streams continuously, so a long gap while connected
    // means the media path died — used by mainLoop to reconnect mid-session.
    std::atomic<TickType_t>                last_rx_pkt_tick_{0};
    // Set by onPeerState on a libpeer-detected drop (Failed/Disconnected/Closed);
    // mainLoop reacts (reconnect or end). Distinct from media-liveness so both
    // a clean ICE close and a silent media death are handled the same way.
    std::atomic<bool>                      peer_dead_{false};
    // Conversation gate: false until the wake word fires, true while a turn is
    // in progress, back to false after a silence timeout. Mic audio is only
    // streamed to the backend while this is true.
    std::atomic<bool>                      conversation_active_{false};
    // Set once the bot's first TTS of a turn arrives, reset on the next wake.
    // While false (awaiting the first reply) user mic energy extends the turn;
    // once true, only the post-reply silence window governs, so ambient room
    // noise can't hold the session open indefinitely after the answer.
    std::atomic<bool>                      bot_replied_{false};
    // A local UI chirp to play (domain::Chirp cast to int, -1 = none). Set by
    // the capture task on wake / end-of-session; consumed and played by the
    // playback task (the sole I2S writer), so there's no cross-task contention
    // on the speaker path. Works even with no peer connection (wake/end happen
    // off-session in the on-demand model).
    std::atomic<int>                       chirp_pending_{-1};
    // Tick by which the turn ends unless pushed forward by user speech (await
    // regime) or a bot TTS frame (post-reply regime). See the capture task.
    std::atomic<TickType_t>                turn_deadline_{0};
    std::atomic<TickType_t>                last_rx_frame_tick_{0};
    std::atomic<TickType_t>                last_mic_active_tick_{0};
    std::atomic<TickType_t>                retry_at_tick_{0};
};

} // namespace app
