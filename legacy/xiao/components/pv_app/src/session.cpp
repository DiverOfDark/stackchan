#include "app/session.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "domain/energy_gate.hpp"
#include "domain/g711.hpp"
#include "domain/gain.hpp"
#include "transport/wake_engine.hpp"

namespace {

constexpr const char* kTag = "session";

// Tuning constants — same values that the legacy webrtc_session.c
// settled on after the energy-gate + watchdog series of fixes.
constexpr int  kRetryIntervalMs       = 5000;
constexpr int  kSessionIdleTimeoutMs  = 10'000;
constexpr int  kSpeakingPcmThreshold  = 1000;     // ~ -30 dBFS
// Drives the TALKING LED only — NOT uplink gating. Turn detection lives on
// the backend (Silero VAD); a device-side energy gate on top of it just
// clipped the quiet start of commands and starved STT.
constexpr int  kMicActiveRmsThreshold = 4000;     // ~ -18 dBFS (post boost)

// Mic input gain (linear), applied with a soft-knee limiter via
// domain::scale_to_i16 so loud speech saturates smoothly instead of
// hard-clipping. gain 1.0 == the old `raw >> 16`.
constexpr float kUplinkGain           = 4.0f;     // +12 dB — healthy STT level
constexpr float kWakeGain             = 8.0f;     // +18 dB — what the model trained on

constexpr int  kAecRefTxChannel       = 0;        // L channel of XVF3800 input

// Half-duplex echo guard: how long after the last inbound TTS frame to keep
// the mic uplink muted. Must outlast the playback-buffer tail (~200 ms) so the
// speaker has gone quiet before we listen again. Prevents the bot hearing
// itself and self-interrupting. See the capture task.
constexpr int  kEchoGuardMs           = 400;

// Conversation turn timeouts. Two regimes so the silence countdown only runs
// AFTER the bot has answered — not during the (variable, sometimes multi-second)
// STT+LLM+TTS round-trip, which used to end the turn before the reply arrived:
//   - while awaiting/receiving the bot's reply (user spoke most recently, or
//     just woke), keep the turn open this long — a safety net for a slow or
//     dead backend, and it bridges gaps between TTS chunks / tool-call pauses;
//   - once the bot's reply finishes, end the turn after this much user silence.
// Each bot TTS frame and each user-speech frame pushes the deadline, so the
// short window only elapses when both have genuinely gone quiet post-reply.
constexpr int  kAwaitResponseMs       = 10000;   // user/bot still expected
constexpr int  kPostResponseSilenceMs = 5000;    // follow-up window after a reply

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
    if (!playback_buf_) {
        ESP_LOGE(kTag, "playback buffer alloc failed");
        running_ = false;
        return;
    }
    // No codec objects: G.711 µ-law is stateless (domain/g711.hpp).

    transport::WakeEngine::initOnce();

    if (!buildAndOffer()) {
        ESP_LOGE(kTag, "initial buildAndOffer failed");
        running_ = false;
        return;
    }

    // Task stacks come from internal RAM. Check the result of every create:
    // xTaskCreate returns pdPASS(1) or errCOULD_NOT_ALLOCATE...(-1), and a
    // silent failure here ran the device with no capture task at all — no mic,
    // no wake, no uplink. (Do NOT fold the results with `&=`: -1 & 1 == 1 reads
    // a failure as success, which is exactly how that bug hid.)
    BaseType_t r_main = xTaskCreatePinnedToCore(mainLoopTaskEntry, "rtc_loop", kMainStack, this, kMainPrio, &t_main_, kMainCore);
    BaseType_t r_cap  = xTaskCreatePinnedToCore(captureTaskEntry,  "rtc_cap",  kCapStack,  this, kCapPrio,  &t_cap_,  kAvCore);
    BaseType_t r_play = xTaskCreatePinnedToCore(playbackTaskEntry, "rtc_play", kPlayStack, this, kPlayPrio, &t_play_, kAvCore);
    if (r_main != pdPASS || r_cap != pdPASS || r_play != pdPASS) {
        ESP_LOGE(kTag, "task create failed (out of internal RAM?) — main=%d cap=%d play=%d",
                 (int)r_main, (int)r_cap, (int)r_play);
    }
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
    last_peer_state_ = static_cast<int>(s);
    switch (s) {
    case PeerState::New:
    case PeerState::Checking:
    case PeerState::Connected:
        ui_.setLed(domain::LedState::Negotiating);
        break;
    case PeerState::Completed:
        connected_            = true;
        conversation_active_  = false;   // idle until the wake word arms a turn
        last_rx_frame_tick_   = 0;
        // Connected but idle — the LED stays Off (driven by the playback tick)
        // until the wake word fires. No green "Listening" glow on connect.
        ui_.setLed(domain::LedState::Off);
        fsm_.onEvent(domain::SessionEvent::PeerLive);
        break;
    case PeerState::Failed:
    case PeerState::Disconnected:
    case PeerState::Closed:
        connected_           = false;
        conversation_active_ = false;
        reconnects_.fetch_add(1);
        retry_at_tick_       = xTaskGetTickCount() + pdMS_TO_TICKS(kRetryIntervalMs);
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
    if (!data || size == 0 || !playback_buf_) return;

    // Inbound is G.711 µ-law @ 8 kHz: one byte per sample. Decode + upsample
    // to 16 kHz mono for the I2S DAC. Cap the payload so 2× upsample can't
    // overflow pcm[].
    static int16_t pcm[domain::kMaxDecodedSamples];
    constexpr std::size_t kMaxBytes = (sizeof(pcm) / sizeof(pcm[0])) / 2;
    if (size > kMaxBytes) size = kMaxBytes;

    const std::size_t samples = domain::g711_decode_to_16k(data, size, pcm);  // = size*2

    xStreamBufferSend(playback_buf_, pcm, samples * sizeof(int16_t), 0);

    if (domain::peak_abs_i16(pcm, static_cast<int>(samples)) >= kSpeakingPcmThreshold) {
        const TickType_t now = xTaskGetTickCount();
        last_rx_frame_tick_ = now;
        // The bot is answering: keep the turn open, and start the (short)
        // post-reply silence countdown from this frame. Each frame pushes it,
        // so it only elapses once the reply has actually stopped.
        turn_deadline_ = now + pdMS_TO_TICKS(kPostResponseSilenceMs);
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
    // Two separate mono buffers at different gains, both soft-limited (no
    // hard clipping — see domain::scale_to_i16). The wake-word detector
    // wants +18 dB because the trained microWakeWord model was fed audio at
    // that level; the uplink path wants a more moderate +12 dB so backend
    // Whisper / Silero get a healthy but undistorted level. Earlier code
    // used one hard-clipped +18 dB buffer for both, which sent square-wave
    // garbage to the backend; the later "raw >> 16" uplink swung the other
    // way and sent audio ~18 dB too quiet for STT to hear.
    static int16_t mono_wake  [domain::kFramesPerPacket];        // +18 dB, soft-limited
    static int16_t mono_uplink[domain::kFramesPerPacket];        // +12 dB, soft-limited
    static uint8_t ulaw_buf   [domain::kFramesPerPacket / 2];    // 8 kHz µ-law on the wire

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
            mono_uplink[i] = domain::scale_to_i16(raw, kUplinkGain);
            mono_wake[i]   = domain::scale_to_i16(raw, kWakeGain);
        }

        const TickType_t now = xTaskGetTickCount();

        // Is the bot playing TTS right now? Used both for echo suppression and
        // to keep the conversation alive while the bot talks. last_rx_frame_
        // tick_ marks the last inbound TTS frame; the guard also spans the
        // playback-buffer tail (~200 ms) still draining to the speaker.
        const TickType_t rx = last_rx_frame_tick_.load();
        const bool bot_speaking = rx != 0 &&
            (now - rx) < pdMS_TO_TICKS(kEchoGuardMs);

        // User mic energy, but only when the bot ISN'T speaking — otherwise the
        // speaker bleed would register as user activity. Feeds the TALKING LED
        // and (while in a turn) extends the await deadline: the user is still
        // talking / about to get a reply, so don't time out.
        const uint32_t rms = domain::rms_i16(mono_wake, domain::kFramesPerPacket);
        if (!bot_speaking && rms >= static_cast<uint32_t>(kMicActiveRmsThreshold)) {
            last_mic_active_tick_ = now;
            if (conversation_active_.load())
                turn_deadline_ = now + pdMS_TO_TICKS(kAwaitResponseMs);
        }

        // Wake word arms the conversation (starts streaming to the backend).
        // Note: do NOT bump last_mic_active_tick_ here — that would make the LED
        // read "Talking" right after the flash; we want "Listening" (go ahead).
        transport::WakeEngine::process(mono_wake, domain::kFramesPerPacket);
        if (transport::WakeEngine::detected()) {
            ui_.setLed(domain::LedState::WakeAck);
            conversation_active_ = true;
            turn_deadline_       = now + pdMS_TO_TICKS(kAwaitResponseMs);
        }

        // End the turn only once the deadline lapses. The deadline is pushed by
        // user speech (above, await regime) and by each inbound bot TTS frame
        // (onInboundAudio, post-reply regime) — so the short post-reply silence
        // window can't elapse until the bot has actually finished answering.
        if (conversation_active_.load()) {
            if (now > turn_deadline_.load()) {
                conversation_active_ = false;
                ESP_LOGI(kTag, "conversation idle — wake word required again");
            }
        }

        if (!connected_.load() || button_.isMuted()) continue;
        if (!conversation_active_.load()) continue;   // listen only after wake
        if (bot_speaking) continue;                   // half-duplex echo guard

        // G.711 encode is ~free, so the capture loop stays comfortably real-time.
        const std::size_t n = domain::g711_encode_16k(
            mono_uplink, domain::kFramesPerPacket, ulaw_buf);   // 320 → 160 bytes
        if (peer_) peer_->sendAudio(ulaw_buf, n);
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
            .conversation_active = conversation_active_.load(),
        });
    }
    vTaskDelete(nullptr);
}

// ---------- Diagnostics ---------------------------------------------------

std::string Session::diagJson()
{
    const TickType_t now = xTaskGetTickCount();
    auto age_ms = [now](const std::atomic<TickType_t>& t) -> long {
        const TickType_t v = t.load();
        return v == 0 ? -1 : static_cast<long>(pdTICKS_TO_MS(now - v));
    };
    auto stack_free = [](TaskHandle_t h) -> unsigned {
        return h ? static_cast<unsigned>(uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t)) : 0;
    };

    const char* pstate = "none";
    switch (last_peer_state_.load()) {
    case static_cast<int>(transport::PeerState::New):          pstate = "new";          break;
    case static_cast<int>(transport::PeerState::Checking):     pstate = "checking";     break;
    case static_cast<int>(transport::PeerState::Connected):    pstate = "connected";    break;
    case static_cast<int>(transport::PeerState::Completed):    pstate = "completed";    break;
    case static_cast<int>(transport::PeerState::Failed):       pstate = "failed";       break;
    case static_cast<int>(transport::PeerState::Disconnected): pstate = "disconnected"; break;
    case static_cast<int>(transport::PeerState::Closed):       pstate = "closed";       break;
    default: break;
    }

    char buf[640];
    snprintf(buf, sizeof buf,
        "{\"uptime_s\":%lld,"
        "\"heap_internal_free\":%u,\"heap_internal_min\":%u,\"psram_free\":%u,"
        "\"connected\":%s,\"peer_state\":\"%s\",\"reconnects\":%u,"
        "\"conversation_active\":%s,\"muted\":%s,"
        "\"ms_since_tts\":%ld,\"ms_since_mic\":%ld,\"wake_p\":%.3f,"
        "\"stack_free_bytes\":{\"main\":%u,\"cap\":%u,\"play\":%u}}",
        static_cast<long long>(esp_timer_get_time() / 1000000),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
        static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
        connected_.load() ? "true" : "false", pstate,
        static_cast<unsigned>(reconnects_.load()),
        conversation_active_.load() ? "true" : "false",
        button_.isMuted() ? "true" : "false",
        age_ms(last_rx_frame_tick_), age_ms(last_mic_active_tick_),
        static_cast<double>(transport::WakeEngine::lastProbability()),
        stack_free(t_main_), stack_free(t_cap_), stack_free(t_play_));
    return std::string(buf);
}

} // namespace app
