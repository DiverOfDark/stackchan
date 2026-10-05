// Adapted from pipecat-voice-assistant pv_app/session.cpp @ f9fd4e7.
// Kept: the on-demand turn model (connect on wake, hang up after the
// reply), the bring-up backlog, echo guard, reconnect budget and turn
// timeouts. Changed: CoreS3 mono audio in place of the XVF3800 stream, no
// LED ring/button/wake-sample upload, push-to-talk, a state/level output
// for the Rust UI, and a WebSocket (raw PCM + JSON events) instead of
// WebRTC/G.722 — see backend/femto_ws.py for the other end.
#include "session.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "domain/audio_frame.hpp"
#include "domain/chirp.hpp"
#include "domain/energy_gate.hpp"
#include "domain/gain.hpp"
#include "domain/led_fsm.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/idf_additions.h"

extern "C" {
#include "wake_word.h"
}

namespace {
constexpr const char* kTag = "voice";
// Give up bring-up (DNS + TCP + TLS + upgrade) after this long, so a failed
// connect doesn't strand the turn.
constexpr int  kConnectTimeoutMs      = 12'000;
// Reconnect attempts within one turn before giving up and ending the session.
constexpr int  kMaxReconnectsPerTurn  = 3;
constexpr int  kSpeakingPcmThreshold  = 1000;     // ~ -30 dBFS
// Drives the "user talking" state only — NOT uplink gating. Turn detection
// lives on the backend (Silero VAD).
constexpr int  kMicActiveRmsThreshold = 4000;     // ~ -18 dBFS (post boost)

// Mic input gain (linear), applied with a soft-knee limiter via
// domain::scale_to_i16 so loud speech saturates smoothly instead of
// hard-clipping.
constexpr float kUplinkGain           = 4.0f;     // +12 dB — healthy STT level
constexpr float kWakeGain             = 8.0f;     // +18 dB — what the model trained on

// Half-duplex echo guard: how long after the last loud frame *played* to
// keep the mic uplink muted, so the bot doesn't hear itself and
// self-interrupt.
constexpr int  kEchoGuardMs           = 400;
// Same for Femto's own chirps (the wake "online" sound): without it the
// backend hears the chirp as speech ("[chime]") and answers it.
constexpr int  kChirpTailMs           = 150;

// Turn timeouts. While awaiting the bot's reply the turn stays open this long
// (a safety net for a slow or dead backend); each played bot frame then
// pushes a shorter post-reply window, which also serves as a hands-free
// follow-up window. Tool calls can leave multi-second gaps mid-answer.
constexpr int  kAwaitResponseMs       = 20000;
constexpr int  kPostResponseSilenceMs = 15000;

constexpr std::size_t kFrameBytes     = domain::kFramesPerPacket * sizeof(int16_t);  // 20 ms
// Uplink backlog: speech during bring-up (and any network stall) waits here,
// in PSRAM. ~8 s.
constexpr std::size_t kUplinkBytes    = kFrameBytes * 400;
// Frames per WebSocket message once connected (catch-up after bring-up).
constexpr std::size_t kSendBatch      = 4;
// Downlink: the backend runs up to 300 ms ahead of real time; the buffer
// holds that plus any Wi-Fi stall. Playback starts once kPrefill is queued.
constexpr std::size_t kPlaybackBytes  = domain::kSampleRateHz * 2 * 3 / 2;   // 1.5 s
constexpr std::size_t kPrefillBytes   = domain::kSampleRateHz * 2 * 12 / 100; // 120 ms
// The tail of a reply shorter than the prefill plays once this quiet.
constexpr int  kPrefillQuietMs        = 80;

constexpr int  kMainStack             = 8 * 1024;
constexpr int  kCapStack              = 24 * 1024;
constexpr int  kPlayStack             = 8 * 1024;
constexpr int  kSendStack             = 6 * 1024;
constexpr int  kMainPrio              = 7;
constexpr int  kCapPrio               = 8;
constexpr int  kPlayPrio              = 8;
constexpr int  kSendPrio              = 7;
constexpr int  kMainCore              = 0;
constexpr int  kAvCore                = 1;
constexpr UBaseType_t kPsram          = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
}  // namespace

namespace femto {

// ---------- Backlog ----------------------------------------------------------

bool Backlog::init(std::size_t capacity)
{
    buf_ = static_cast<uint8_t*>(heap_caps_malloc(capacity, kPsram));
    cap_ = buf_ ? capacity : 0;
    return buf_ != nullptr;
}

void Backlog::push(const void* data, std::size_t len)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const auto* p = static_cast<const uint8_t*>(data);
    for (std::size_t i = 0; i < len && cap_; ++i) {
        buf_[(head_ + count_) % cap_] = p[i];
        if (count_ < cap_) count_++;
        else head_ = (head_ + 1) % cap_;   // full: drop the oldest byte
    }
}

std::size_t Backlog::pop(void* out, std::size_t max)
{
    std::lock_guard<std::mutex> lk(mtx_);
    const std::size_t n = std::min(max, count_);
    auto* o = static_cast<uint8_t*>(out);
    for (std::size_t i = 0; i < n; ++i) o[i] = buf_[(head_ + i) % cap_];
    head_ = cap_ ? (head_ + n) % cap_ : 0;
    count_ -= n;
    return n;
}

void Backlog::clear()
{
    std::lock_guard<std::mutex> lk(mtx_);
    head_ = count_ = 0;
}

// ---------- Session ----------------------------------------------------------

Session::Session(std::string backend_url, AudioCores3& audio)
    : backend_url_(std::move(backend_url)), audio_(audio) {}

void Session::start()
{
    if (running_.exchange(true)) return;
    playback_buf_ = xStreamBufferCreateWithCaps(kPlaybackBytes, kFrameBytes, kPsram);
    if (!playback_buf_ || !uplink_.init(kUplinkBytes)) {
        ESP_LOGE(kTag, "audio buffers: out of PSRAM");
        running_ = false;
        return;
    }
    wake_word_init();
    // Stacks in PSRAM: internal RAM is spoken for (Wi-Fi, camera, display).
    // None of these tasks touch flash, so the stacks stay reachable.
    BaseType_t a = xTaskCreatePinnedToCoreWithCaps(mainLoopTaskEntry, "voice_loop", kMainStack, this, kMainPrio, nullptr, kMainCore, kPsram);
    BaseType_t b = xTaskCreatePinnedToCoreWithCaps(captureTaskEntry, "voice_cap", kCapStack, this, kCapPrio, nullptr, kAvCore, kPsram);
    BaseType_t c = xTaskCreatePinnedToCoreWithCaps(playbackTaskEntry, "voice_play", kPlayStack, this, kPlayPrio, nullptr, kAvCore, kPsram);
    BaseType_t d = xTaskCreatePinnedToCoreWithCaps(senderTaskEntry, "voice_send", kSendStack, this, kSendPrio, nullptr, kMainCore, kPsram);
    if (a != pdPASS || b != pdPASS || c != pdPASS || d != pdPASS)
        ESP_LOGE(kTag, "task create failed main=%d cap=%d play=%d send=%d", (int)a, (int)b, (int)c, (int)d);
}

std::shared_ptr<WsLink> Session::link()
{
    std::lock_guard<std::mutex> lk(link_mtx_);
    return link_;
}

bool Session::connect()
{
    auto l = std::make_shared<WsLink>(
        [this](const int16_t* pcm, std::size_t n) { onInboundAudio(pcm, n); },
        [this](std::string json) { onText(std::move(json)); });
    const std::string url = ws_url_for(backend_url_);
    ESP_LOGI(kTag, "connecting %s", url.c_str());
    if (!l->start(url)) return false;
    std::lock_guard<std::mutex> lk(link_mtx_);
    link_ = std::move(l);
    return true;
}

void Session::disconnect()
{
    connected_ = false;
    std::shared_ptr<WsLink> old;
    {
        std::lock_guard<std::mutex> lk(link_mtx_);
        old = std::move(link_);
    }
    // Destroyed here or, if the sender still holds it mid-send, right after.
}

void Session::onLinkUp()
{
    // Session metadata first: the backend builds the pipeline from it.
    auto l = link();
    if (!l || !l->sendText(hello_, 2000)) {
        link_dead_ = true;
        return;
    }
    connected_          = true;
    last_rx_frame_tick_ = 0;
    last_rx_pkt_tick_   = xTaskGetTickCount();
    // Restart the turn clock at connect: bring-up may have eaten part of the
    // window the wake word set, and speech buffered during bring-up won't bump
    // it again.
    turn_deadline_      = xTaskGetTickCount() + pdMS_TO_TICKS(kAwaitResponseMs);
    fsm_.onEvent(domain::SessionEvent::PeerLive);
    ESP_LOGI(kTag, "session up");
}

void Session::onInboundAudio(const int16_t* pcm, std::size_t samples)
{
    last_rx_pkt_tick_ = xTaskGetTickCount();
    const uint32_t n = rx_audio_chunks_.fetch_add(1) + 1;
    if ((n % 200) == 1)
        ESP_LOGI(kTag, "rx audio: chunk#%u queued=%u", (unsigned)n, (unsigned)xStreamBufferBytesAvailable(playback_buf_));
    // Blocks only if 1.5 s is already queued — backpressure onto TCP.
    xStreamBufferSend(playback_buf_, pcm, samples * sizeof(int16_t), pdMS_TO_TICKS(200));
}

void Session::onText(std::string json)
{
    if (json.find("\"thinking\"") != std::string::npos) {
        // The backend is still working on a reply (the agent LLM can take
        // 15–40 s on a tool call): keep the turn open while it says so.
        const TickType_t until = xTaskGetTickCount() + pdMS_TO_TICKS(kAwaitResponseMs);
        if (until > turn_deadline_.load()) turn_deadline_ = until;
    }
    if (json.find("\"interrupted\"") != std::string::npos) {
        // Barge-in: the queued reply is stale. Safe to reset here: the
        // playback task never blocks on the buffer, and this task is its only
        // writer.
        xStreamBufferReset(playback_buf_);
        last_rx_frame_tick_ = 0;
    }
    std::lock_guard<std::mutex> lk(events_mtx_);
    if (events_.size() >= 64) events_.pop_front();
    events_.push_back(std::move(json));
}

bool Session::nextEvent(std::string& out)
{
    std::lock_guard<std::mutex> lk(events_mtx_);
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

void Session::mainLoopTaskEntry(void* arg) { static_cast<Session*>(arg)->mainLoopTask(); }
void Session::captureTaskEntry (void* arg) { static_cast<Session*>(arg)->captureTask(); }
void Session::playbackTaskEntry(void* arg) { static_cast<Session*>(arg)->playbackTask(); }
void Session::senderTaskEntry  (void* arg) { static_cast<Session*>(arg)->senderTask(); }

void Session::mainLoopTask()
{
    TickType_t connect_started = 0;
    bool       prev_want       = false;
    int        reconnects      = 0;   // reconnects used this turn

    while (running_.load()) {
        const TickType_t now  = xTaskGetTickCount();
        const bool       want = conversation_active_.load();
        auto             l    = link();
        if (want && !prev_want) {
            reconnects = 0;
            // Radio power save adds ~100 ms latency spikes; off for the turn.
            esp_wifi_set_ps(WIFI_PS_NONE);
        } else if (!want && prev_want) {
            esp_wifi_set_ps(WIFI_PS_MIN_MODEM);
        }
        prev_want = want;

        if (want && !l) {
            // A fresh wake, or a rebuild after a drop. The capture task is
            // already buffering the user's speech into the backlog.
            ESP_LOGI(kTag, "%s", reconnects ? "reconnecting" : "wake → connecting");
            connect_started = now;
            link_dead_      = false;
            if (!connect()) {
                ESP_LOGE(kTag, "connect failed; abandoning turn");
                conversation_active_ = false;
                disconnect();
            }
        } else if (!want && l) {
            // Turn over → hang up; the backend reaps its pipeline on close.
            ESP_LOGI(kTag, "conversation ended → disconnecting");
            disconnect();
        } else if (want && l) {
            if (!connected_.load() && l->open()) onLinkUp();
            if (link_dead_.load() || l->dead()) {
                if (reconnects < kMaxReconnectsPerTurn) {
                    ++reconnects;
                    ESP_LOGW(kTag, "connection lost → reconnect %d/%d", reconnects, kMaxReconnectsPerTurn);
                    disconnect();   // next iteration rebuilds
                } else {
                    ESP_LOGW(kTag, "connection lost; reconnects exhausted → ending session");
                    conversation_active_ = false;
                }
            } else if (!connected_.load() && (now - connect_started) > pdMS_TO_TICKS(kConnectTimeoutMs)) {
                ESP_LOGW(kTag, "connect timed out; abandoning turn");
                conversation_active_ = false;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(nullptr);
}

// ---------- Sender task: backlog → WebSocket ---------------------------------

void Session::senderTask()
{
    uint8_t* batch = static_cast<uint8_t*>(heap_caps_malloc(kFrameBytes * kSendBatch, kPsram));
    while (running_.load()) {
        auto l = connected_.load() ? link() : nullptr;
        const std::size_t n = l ? uplink_.pop(batch, kFrameBytes * kSendBatch) : 0;
        if (n == 0) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        // A stall here only delays the uplink: the capture task keeps filling
        // the backlog, so no microphone audio is lost.
        if (!l->sendBinary(batch, n, 3000)) {
            ESP_LOGW(kTag, "uplink send failed");
            link_dead_ = true;
        }
    }
    heap_caps_free(batch);
    vTaskDelete(nullptr);
}

// ---------- Capture task (CoreS3 mono mic) ---------------------------------

void Session::captureTask()
{
    static int16_t raw[domain::kFramesPerPacket];
    static int16_t mono_wake[domain::kFramesPerPacket];
    static int16_t mono_uplink[domain::kFramesPerPacket];
    static const int16_t zero_pcm[domain::kFramesPerPacket] = {0};
    constexpr int kRearmFrames = 50;   // 1 s of 20 ms frames
    int rearm_frames = 0;

    while (running_.load()) {
        if (audio_.read(raw, domain::kFramesPerPacket) != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // ES7210 already applies analog gain; the wake model wants a hotter
        // signal than STT, as on the original board (+6 dB over uplink).
        for (std::size_t i = 0; i < domain::kFramesPerPacket; ++i) {
            const int32_t q31 = static_cast<int32_t>(raw[i]) << 16;
            mono_uplink[i] = domain::scale_to_i16(q31, kUplinkGain / 4.0f);
            mono_wake[i]   = domain::scale_to_i16(q31, kWakeGain / 4.0f);
        }

        const TickType_t now = xTaskGetTickCount();
        const TickType_t rx = last_rx_frame_tick_.load();
        const bool bot_speaking = (rx != 0 && (now - rx) < pdMS_TO_TICKS(kEchoGuardMs)) ||
                                  now < chirp_mute_until_.load();

        const uint32_t rms = domain::rms_i16(mono_wake, domain::kFramesPerPacket);
        mic_level_ = std::min(1.0f, rms / 8000.0f);
        if (!bot_speaking && rms >= static_cast<uint32_t>(kMicActiveRmsThreshold)) {
            last_mic_active_tick_ = now;
            if (conversation_active_.load() && !bot_replied_.load())
                turn_deadline_ = now + pdMS_TO_TICKS(kAwaitResponseMs);
        }

        // Wake word only while armed (someone's been around lately); the
        // model isn't fed otherwise. After re-arming, ignore it for a second
        // so the stale end of its sliding window can't fire.
        bool heard = false;
        if (wake_armed_.load()) {
            wake_word_process(mono_wake, domain::kFramesPerPacket);
            heard = wake_word_detected() && rearm_frames == 0;
            if (rearm_frames > 0) --rearm_frames;
        } else {
            rearm_frames = kRearmFrames;
        }
        const bool woke = heard || ptt_.exchange(false);
        if (woke) {
            if (!conversation_active_.exchange(true)) {
                ESP_LOGI(kTag, "wake → turn armed");
                uplink_.clear();
                bot_replied_ = false;
                chirp_pending_ = static_cast<int>(domain::Chirp::Wake);
            }
            turn_deadline_ = now + pdMS_TO_TICKS(kAwaitResponseMs);
        }

        if (conversation_active_.load() && now > turn_deadline_.load()) {
            conversation_active_ = false;
            chirp_pending_ = static_cast<int>(domain::Chirp::End);
            ESP_LOGI(kTag, "conversation idle — wake word required again");
        }
        if (!conversation_active_.load()) continue;

        // Silence while the bot talks (echo guard), so the stream stays
        // continuous for the backend's VAD.
        uplink_.push(bot_speaking ? zero_pcm : mono_uplink, kFrameBytes);
    }
    vTaskDelete(nullptr);
}

// ---------- Playback task ---------------------------------------------------

void Session::playbackTask()
{
    static int16_t mono[domain::kFramesPerPacket];
    int16_t* chirp = static_cast<int16_t*>(heap_caps_malloc(domain::kChirpMaxSamples * sizeof(int16_t), kPsram));
    bool primed = false;

    while (running_.load()) {
        const int ch = chirp_pending_.exchange(-1);
        if (ch >= 0 && chirp) {
            const std::size_t cn = domain::synth_chirp(static_cast<domain::Chirp>(ch), chirp, domain::kChirpMaxSamples);
            chirp_mute_until_ = xTaskGetTickCount() + pdMS_TO_TICKS(cn * 1000 / domain::kSampleRateHz + kChirpTailMs);
            audio_.write(chirp, cn);
        }
        // Never block on the buffer (onText may reset it); the I2S write
        // below paces this loop at real time.
        const TickType_t now = xTaskGetTickCount();
        if (!primed) {
            const std::size_t avail = xStreamBufferBytesAvailable(playback_buf_);
            const bool quiet = (now - last_rx_pkt_tick_.load()) > pdMS_TO_TICKS(kPrefillQuietMs);
            primed = avail >= kPrefillBytes || (avail > 0 && quiet);
        }
        const std::size_t got = primed ? xStreamBufferReceive(playback_buf_, mono, sizeof(mono), 0) : 0;
        if (got < sizeof(mono)) {
            std::memset(reinterpret_cast<uint8_t*>(mono) + got, 0, sizeof(mono) - got);
            primed = false;   // ran dry: re-buffer before playing on
        }

        const int32_t peak_raw = domain::peak_abs_i16(mono, domain::kFramesPerPacket);
        if (peak_raw >= kSpeakingPcmThreshold) {
            // The bot is audibly answering (what's played, not what's
            // queued): feeds the echo guard and keeps the turn open.
            last_rx_frame_tick_ = now;
            bot_replied_ = true;
            turn_deadline_ = now + pdMS_TO_TICKS(kPostResponseSilenceMs);
        }
        // Mouth envelope: fast attack, slower release.
        const float peak = peak_raw / 12000.0f;
        const float prev = level_.load();
        level_ = std::min(1.0f, peak > prev ? peak : prev * 0.8f + peak * 0.2f);
        audio_.write(mono, domain::kFramesPerPacket);
        updateState();
    }
    if (chirp) heap_caps_free(chirp);
    vTaskDelete(nullptr);
}

/// Same rules as the original LED ring (domain::resolveLedState), mapped to
/// Femto's screens.
void Session::updateState()
{
    if (!conversation_active_.load()) {
        state_ = FEMTO_VOICE_IDLE;
        return;
    }
    if (!connected_.load()) {
        state_ = FEMTO_VOICE_CONNECTING;
        return;
    }
    const auto ms = [](TickType_t t) { return domain::Ms{pdTICKS_TO_MS(t)}; };
    auto led = domain::resolveLedState({
        .now = ms(xTaskGetTickCount()),
        .last_inbound_audio = ms(last_rx_frame_tick_.load()),
        .last_mic_active = ms(last_mic_active_tick_.load()),
        .connected = true,
        .muted = false,
        .conversation_active = true,
    });
    switch (led.value_or(domain::LedState::Listening)) {
    case domain::LedState::Speaking: state_ = FEMTO_VOICE_SPEAKING; break;
    case domain::LedState::Thinking: state_ = FEMTO_VOICE_THINKING; break;
    default: state_ = FEMTO_VOICE_LISTENING; break;
    }
}

}  // namespace femto
