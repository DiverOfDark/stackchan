#include "app/session.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "esp_log.h"
#include "esp_timer.h"

#include "domain/energy_gate.hpp"
#include "transport/wake_engine.hpp"

namespace {

constexpr const char* kTag = "session";

// Tuning constants — same values that the legacy webrtc_session.c
// settled on after the energy-gate + watchdog series of fixes.
constexpr int  kRetryIntervalMs       = 5000;
constexpr int  kSessionIdleTimeoutMs  = 10'000;
constexpr int  kSpeakingPcmThreshold  = 1000;     // ~ -30 dBFS
constexpr int  kMicActiveRmsThreshold = 4000;     // ~ -18 dBFS (post 18 dB boost)
constexpr int  kUplinkHoldMs          = 2000;     // device-side VAD hysteresis

constexpr int  kAecRefTxChannel       = 0;        // L channel of XVF3800 input

constexpr int  kMainStack             = 32 * 1024;
constexpr int  kCapStack              = 32 * 1024;
constexpr int  kPlayStack             = 16 * 1024;
// Priorities mirror the legacy webrtc_session.c values (7/8/8) —
// running too low lets the IDLE task starve audio servicing and
// surfaces as a watchdog within the DTLS handshake path.
constexpr int  kMainPrio              = 7;
constexpr int  kCapPrio               = 8;
constexpr int  kPlayPrio              = 8;
constexpr int  kMainCore              = 0;
constexpr int  kAvCore                = 1;

constexpr std::size_t kPlaybackBufBytes = domain::kSampleRateHz * 2 / 5;  // 200 ms @ 16 kHz mono
// Trigger level = one full 20 ms packet (640 bytes of int16 PCM). Anything
// smaller wakes the playback task on a partial read and pads the rest with
// silence — the half-packet "click click click" stream that comes out of
// the speaker poisons the XVF3800's AEC reference, so user speech reaches
// the backend mixed with un-cancelled bot echo and Whisper transcribes
// garbage. Bug caught by an STT regression after the C → C++ port.
constexpr std::size_t kPlaybackBufTrig  = domain::kFramesPerPacket * sizeof(int16_t);

} // namespace

namespace app {

Session::Session(std::string backend_url,
                 hal::Xvf3800& ring,
                 hal::AudioIo& audio,
                 hal::Button&  button,
                 std::vector<transport::PeerIceServer> ice)
    : backend_url_(std::move(backend_url))
    , ring_(ring)
    , audio_(audio)
    , button_(button)
    , ice_(std::move(ice))
    , ui_(ring_)
    , signaling_(backend_url_)
{}

Session::~Session()
{
    stop();
    if (playback_buf_) vStreamBufferDelete(playback_buf_);
}

void Session::start()
{
    if (running_.exchange(true)) return;

    // Initialise libpeer (libsrtp + usrsctp globals) before any
    // Peer::create. Without this srtp_unprotect dereferences a NULL
    // global context on the first inbound packet and panics.
    if (transport::Peer::initLibpeerOnce() != ESP_OK) {
        ESP_LOGE(kTag, "libpeer init failed");
        running_ = false;
        return;
    }

    playback_buf_ = xStreamBufferCreate(kPlaybackBufBytes, kPlaybackBufTrig);
    auto e = transport::OpusEncoder::create();
    auto d = transport::OpusDecoder::create();
    if (!e || !d || !playback_buf_) {
        ESP_LOGE(kTag, "codec / buffer alloc failed");
        running_ = false;
        return;
    }
    enc_ = std::make_unique<transport::OpusEncoder>(std::move(*e));
    dec_ = std::make_unique<transport::OpusDecoder>(std::move(*d));

    transport::WakeEngine::initOnce();

    if (!buildAndOffer()) {
        ESP_LOGE(kTag, "initial buildAndOffer failed");
        running_ = false;
        return;
    }

    xTaskCreatePinnedToCore(mainLoopTaskEntry, "rtc_loop", kMainStack, this, kMainPrio, &t_main_, kMainCore);
    xTaskCreatePinnedToCore(captureTaskEntry,  "rtc_cap",  kCapStack,  this, kCapPrio,  &t_cap_,  kAvCore);
    xTaskCreatePinnedToCore(playbackTaskEntry, "rtc_play", kPlayStack, this, kPlayPrio, &t_play_, kAvCore);
}

void Session::stop()
{
    if (!running_.exchange(false)) return;
    // Tasks see running_=false and exit; give them a tick to wind down,
    // then null the handles. (vTaskDelete from another task races with
    // a self-exit; we prefer the self-exit path.)
    vTaskDelay(pdMS_TO_TICKS(50));
    t_main_ = t_cap_ = t_play_ = nullptr;
    peer_.reset();
}

bool Session::buildAndOffer()
{
    peer_.reset();
    peer_ = transport::Peer::create(ice_);
    if (!peer_) return false;

    peer_->setOnStateChange([this](transport::PeerState s) { onPeerState(s); });
    peer_->setOnLocalSdp   ([this](std::string sdp)        { onLocalSdp(std::move(sdp)); });
    peer_->setOnAudio      ([this](const uint8_t* d, std::size_t n) { onInboundAudio(d, n); });

    const char* offer = peer_->createOffer();
    if (!offer) {
        ESP_LOGE(kTag, "createOffer returned null");
        return false;
    }
    // createOffer fires the on_local_sdp callback synchronously — by
    // now pending_offer_for_signaling_ is populated. Drain on the
    // main loop next tick.
    return true;
}

// ---------- libpeer callbacks --------------------------------------------

void Session::onPeerState(transport::PeerState s)
{
    // NOTE: don't introduce a `using PS = ...` alias here — `PS` is a
    // hardware register name in xtensa/config/specreg.h and the
    // macros from that header clash with any local PS identifier.
    using transport::PeerState;
    switch (s) {
    case PeerState::New:
    case PeerState::Checking:
    case PeerState::Connected:
        ui_.setLed(domain::LedState::Negotiating);
        break;
    case PeerState::Completed:
        connected_         = true;
        last_mic_active_tick_ = xTaskGetTickCount();   // auto-arm uplink
        last_rx_frame_tick_   = 0;
        ui_.setLed(domain::LedState::Listening);
        fsm_.onEvent(domain::SessionEvent::PeerLive);
        break;
    case PeerState::Failed:
    case PeerState::Disconnected:
    case PeerState::Closed:
        connected_         = false;
        retry_at_tick_     = xTaskGetTickCount() + pdMS_TO_TICKS(kRetryIntervalMs);
        ui_.setLed(domain::LedState::Connecting);
        fsm_.onEvent(domain::SessionEvent::PeerLost);
        break;
    }
}

void Session::onLocalSdp(std::string sdp)
{
    // Fired synchronously from libpeer inside createOffer(), before
    // any worker task runs. Do the signaling POST RIGHT HERE so the
    // answer is parked on the Peer before the main loop starts —
    // otherwise libpeer spends ~2 s spinning without a remote
    // description, which we discovered crashes the SRTP path in
    // unexpected ways the first time DTLS state advances.
    auto resp = signaling_.sendOffer(sdp);
    if (!resp || !peer_) {
        ESP_LOGE(kTag, "signaling.sendOffer failed in onLocalSdp; scheduling retry");
        retry_at_tick_ = xTaskGetTickCount() + pdMS_TO_TICKS(kRetryIntervalMs);
        return;
    }
    peer_->publishAnswer(std::move(resp->remote_sdp));
}

void Session::onInboundAudio(const uint8_t* data, std::size_t size)
{
    if (!data || size == 0 || size > 1500 || !dec_ || !playback_buf_) return;

    static int16_t pcm[domain::kOpusMaxDecodedSamples];
    int decoded = dec_->decode(data, size, pcm, sizeof(pcm) / sizeof(pcm[0]));
    if (decoded <= 0) return;

    const std::size_t bytes = static_cast<std::size_t>(decoded) * sizeof(int16_t);
    xStreamBufferSend(playback_buf_, pcm, bytes, 0);

    if (domain::peak_abs_i16(pcm, decoded) >= kSpeakingPcmThreshold) {
        last_rx_frame_tick_ = xTaskGetTickCount();
    }
}

// ---------- Main loop task -----------------------------------------------

void Session::mainLoopTaskEntry(void* arg) { static_cast<Session*>(arg)->mainLoopTask(); }
void Session::captureTaskEntry (void* arg) { static_cast<Session*>(arg)->captureTask(); }
void Session::playbackTaskEntry(void* arg) { static_cast<Session*>(arg)->playbackTask(); }

void Session::mainLoopTask()
{
    while (running_.load()) {
        if (peer_) peer_->tick();

        // Retry timer expired → rebuild the peer from scratch.
        const TickType_t target = retry_at_tick_.load();
        if (target && xTaskGetTickCount() >= target) {
            retry_at_tick_ = 0;
            ESP_LOGI(kTag, "retry timer fired; rebuilding peer");
            fsm_.onEvent(domain::SessionEvent::RetryTick);
            buildAndOffer();
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(nullptr);
}

// ---------- Capture task --------------------------------------------------

void Session::captureTask()
{
    static int32_t stereo[domain::kFramesPerPacket * hal::AudioIo::kChannels];
    // Two separate mono buffers: the wake-word detector needs the +18 dB
    // software boost (the trained microWakeWord model was fed audio at
    // that level) while the uplink path wants clean unboosted audio so
    // backend Whisper / Silero see what the XVF3800's AEC + AGC actually
    // produced. Earlier code used one boosted buffer for both, which
    // sent over-amplified, often clipped audio to the backend — STT
    // transcribed it as garbage.
    static int16_t mono_wake  [domain::kFramesPerPacket];   // post >>13 boost
    static int16_t mono_uplink[domain::kFramesPerPacket];   // post >>16, raw level
    static uint8_t opus_buf[domain::kOpusMaxPacketBytes];

    while (running_.load()) {
        if (audio_.read(stereo, domain::kFramesPerPacket) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // XVF3800 default output: channel 0 (left) = auto-select beam
        // (processed mono); channel 1 (right) = silence. Per the
        // reSpeaker_XVF3800_USB_4MIC_ARRAY host_control docs.
        for (std::size_t i = 0; i < domain::kFramesPerPacket; ++i) {
            const int32_t raw = stereo[i * hal::AudioIo::kChannels];
            // Uplink: take the top 16 bits — the conventional 32→16
            // bit reduction, matches what ESPHome's i2s_audio does on
            // the same hardware.
            mono_uplink[i] = static_cast<int16_t>(raw >> 16);
            // Wake word: +18 dB boost (>>13 + clamp) so the trained
            // model sees voice at the 30-50 % level it expects.
            int32_t boosted = raw >> 13;
            if (boosted >  INT16_MAX) boosted = INT16_MAX;
            if (boosted <  INT16_MIN) boosted = INT16_MIN;
            mono_wake[i] = static_cast<int16_t>(boosted);
        }

        // Local mic-RMS gate uses the boosted buffer (matches the
        // threshold tuning that was done against the boosted levels).
        const uint32_t rms = domain::rms_i16(mono_wake, domain::kFramesPerPacket);
        if (rms >= static_cast<uint32_t>(kMicActiveRmsThreshold)) {
            last_mic_active_tick_ = xTaskGetTickCount();
        }

        transport::WakeEngine::process(mono_wake, domain::kFramesPerPacket);
        if (transport::WakeEngine::detected()) {
            ui_.setLed(domain::LedState::WakeAck);
            last_mic_active_tick_ = xTaskGetTickCount();
        }

        if (!connected_.load() || button_.isMuted()) continue;

        if ((xTaskGetTickCount() - last_mic_active_tick_.load()) >
                pdMS_TO_TICKS(kUplinkHoldMs)) {
            continue;
        }

        int n = enc_->encode(mono_uplink, domain::kFramesPerPacket,
                             opus_buf, sizeof(opus_buf));
        if (n > 0 && peer_) {
            peer_->sendAudio(opus_buf, static_cast<std::size_t>(n));
        }
    }
    vTaskDelete(nullptr);
}

// ---------- Playback task -------------------------------------------------

void Session::playbackTask()
{
    static int16_t mono  [domain::kFramesPerPacket];
    static int32_t stereo[domain::kFramesPerPacket * hal::AudioIo::kChannels];

    while (running_.load()) {
        std::size_t got = playback_buf_
            ? xStreamBufferReceive(playback_buf_, mono, sizeof(mono),
                                   pdMS_TO_TICKS(50))
            : 0;
        std::size_t frames = got / sizeof(int16_t);
        if (frames == 0) {
            std::memset(mono, 0, sizeof(mono));
            frames = domain::kFramesPerPacket;
        } else if (frames < domain::kFramesPerPacket) {
            std::memset(mono + frames, 0, sizeof(mono) - got);
            frames = domain::kFramesPerPacket;
        }
        // Mono int16 → stereo int32 MSB-aligned. TTS on the AEC-ref
        // channel, silence on the other.
        for (std::size_t i = 0; i < frames; ++i) {
            int32_t sample = static_cast<int32_t>(mono[i]) << 16;
            stereo[i * hal::AudioIo::kChannels + kAecRefTxChannel]       = sample;
            stereo[i * hal::AudioIo::kChannels + (1 - kAecRefTxChannel)] = 0;
        }
        audio_.write(stereo, frames);

        ui_.tick({
            .now                 = std::chrono::milliseconds{pdTICKS_TO_MS(xTaskGetTickCount())},
            .last_inbound_audio  = std::chrono::milliseconds{pdTICKS_TO_MS(last_rx_frame_tick_.load())},
            .last_mic_active     = std::chrono::milliseconds{pdTICKS_TO_MS(last_mic_active_tick_.load())},
            .connected           = connected_.load(),
            .muted               = button_.isMuted(),
        });
    }
    vTaskDelete(nullptr);
}

} // namespace app
