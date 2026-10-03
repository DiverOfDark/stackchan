// Adapted from pipecat-voice-assistant pv_app/session.cpp @ f9fd4e7.
// Unchanged: onPeerState, onLocalSdp, onInboundAudio, buildAndOffer,
// mainLoopTask and the capture/turn logic. Changed: CoreS3 mono audio in
// place of the XVF3800 32-bit stereo stream, no LED ring/button/wake-sample
// upload, push-to-talk, and a state/level output for the Rust UI.
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
#include "freertos/idf_additions.h"
#include "esp_log.h"
#include "transport/wake_engine.hpp"

namespace {
constexpr const char* kTag = "voice";
// Tuning constants — same values that the legacy webrtc_session.c
// settled on after the energy-gate + watchdog series of fixes.
constexpr int  kRetryIntervalMs       = 5000;
constexpr int  kSessionIdleTimeoutMs  = 10'000;
// On-demand connect: give up bring-up if relay/ICE/DTLS doesn't reach
// Completed within this window, so a failed connect doesn't strand the turn.
constexpr int  kConnectTimeoutMs      = 12'000;
// A healthy backend streams downlink audio continuously (~50 pkts/s, silence
// between TTS). No inbound packet for this long while connected ⇒ the media
// path is dead even if ICE consent still trickles through the relay — trigger
// a reconnect. Generous so brief jitter never false-triggers it.
constexpr int  kMediaDeadMs           = 5'000;
// Reconnect attempts within one turn before giving up and ending the session.
// Bounds a flapping/broken relay so it can't loop forever.
constexpr int  kMaxReconnectsPerTurn  = 3;
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

// Wake-trigger capture: how much mic audio (mono_uplink, 16 kHz int16) to keep
// rolling so a fire can be snapshotted with the audio that caused it. The fire
// lands at the END of this buffer, so it holds the triggering phrase (positive
// clips are a 1.5 s window) plus ~1.5 s of lead-in context — useful for both
// labelling and the training slide-window (clip 1.5 s / aug 3.2 s).
constexpr int          kWakeSampleRate      = 16000;
constexpr std::size_t  kWakeCaptureSamples  = kWakeSampleRate * 3;   // 3 s = 96 KB PSRAM



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
// Window to wait for the bot's first reply. On-demand connect adds ~4-5 s of
// relay/ICE/DTLS bring-up plus the buffered-utterance flush before the backend
// even hears the question, then STT+LLM+TTS — the first audio can land ~13 s
// after connect. Generous so we don't tear the turn down right before the
// answer; reset when the peer reaches Completed (see onPeerState) so the clock
// starts at connect, not at the user's speech during bring-up.
constexpr int  kAwaitResponseMs       = 20000;   // user/bot still expected
// Gap tolerance after a bot TTS chunk. The reply is multi-part — narration →
// tool call → answer sentences — with 4-5 s (sometimes much longer) silent gaps
// while a tool runs. At 5 s the device tore the session down inside those gaps
// and lost the rest of the answer (confirmed: hung up exactly 5 s after the last
// loud frame). 15 s comfortably bridges inter-sentence + typical tool gaps and
// doubles as a hands-free follow-up window. (A backend end-of-turn signal over a
// data channel would let us shorten this — see CLAUDE.md open items.)
constexpr int  kPostResponseSilenceMs = 15000;   // bridge tool/inter-sentence gaps + follow-up


constexpr int  kMainStack             = 16 * 1024;
constexpr int  kCapStack              = 24 * 1024;
constexpr int  kPlayStack             = 8 * 1024;
constexpr int  kMainPrio              = 7;
constexpr int  kCapPrio               = 8;
constexpr int  kPlayPrio              = 8;
constexpr int  kMainCore              = 0;
constexpr int  kAvCore                = 1;
constexpr std::size_t kPlaybackBufBytes = domain::kSampleRateHz * 2 / 5;
constexpr std::size_t kPlaybackBufTrig  = domain::kFramesPerPacket * sizeof(int16_t);
}  // namespace

namespace femto {
using namespace ::transport;
namespace transport = ::transport;

Session::Session(std::string backend_url, AudioCores3& audio)
    : backend_url_(std::move(backend_url)), audio_(audio), signaling_(backend_url_) {}

void Session::start()
{
    if (running_.exchange(true)) return;
    if (transport::Peer::initLibpeerOnce() != ESP_OK) {
        ESP_LOGE(kTag, "libpeer init failed");
        running_ = false;
        return;
    }
    playback_buf_ = xStreamBufferCreate(kPlaybackBufBytes, kPlaybackBufTrig);
    transport::WakeEngine::initOnce();
    // Stacks in PSRAM: internal RAM is spoken for (Wi-Fi, camera, display).
    // None of these tasks touch flash, so the stacks stay reachable.
    constexpr UBaseType_t kStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    BaseType_t a = xTaskCreatePinnedToCoreWithCaps(mainLoopTaskEntry, "rtc_loop", kMainStack, this, kMainPrio, nullptr, kMainCore, kStackCaps);
    BaseType_t b = xTaskCreatePinnedToCoreWithCaps(captureTaskEntry, "rtc_cap", kCapStack, this, kCapPrio, nullptr, kAvCore, kStackCaps);
    BaseType_t c = xTaskCreatePinnedToCoreWithCaps(playbackTaskEntry, "rtc_play", kPlayStack, this, kPlayPrio, nullptr, kAvCore, kStackCaps);
    if (a != pdPASS || b != pdPASS || c != pdPASS)
        ESP_LOGE(kTag, "task create failed (internal RAM?) main=%d cap=%d play=%d", (int)a, (int)b, (int)c);
}

bool Session::buildAndOffer()
{
    // The STUNner TURN credentials are fetched once at boot. If that fetch
    // failed (e.g. the backend was restarting), ice_ is empty — and with no
    // relay the device can't reach the in-cluster backend (its pod IP isn't
    // LAN-routable), so ICE never completes and the connect times out. Re-fetch
    // lazily here so a bad boot fetch / backend restart self-heals on the next
    // wake instead of stranding the device until a reboot. Only when empty, so
    // the happy path adds no latency.
    if (ice_.empty()) {
        ESP_LOGW(kTag, "no ICE servers cached — re-fetching from backend");
        auto raw = signaling_.fetchIceServers();
        ice_.clear();
        ice_.reserve(raw.size());
        for (auto& s : raw)
            ice_.push_back({std::move(s.url), std::move(s.username), std::move(s.credential)});
        ESP_LOGI(kTag, "ICE: %u server(s) after re-fetch", (unsigned)ice_.size());
    }

    peer_.reset();
    peer_ = transport::Peer::create(ice_);
    if (!peer_) return false;

    // Fresh inbound G.722 stream for this connection. (The uplink encoder is
    // reset by the capture task when the wake word starts a new utterance, so
    // the buffered head-start and the live audio stay one continuous stream.)
    domain::g722_init(g722_dec_);

    peer_->setOnStateChange([this](transport::PeerState s) { onPeerState(s); });
    peer_->setOnLocalSdp   ([this](std::string sdp)        { onLocalSdp(std::move(sdp)); });
    peer_->setOnAudio      ([this](const uint8_t* d, std::size_t n) { onInboundAudio(d, n); });
    peer_->setOnData([this](const char* d, std::size_t n) {
        std::lock_guard<std::mutex> lk(events_mtx_);
        if (events_.size() >= 64) events_.pop_front();
        events_.emplace_back(d, n);
    });

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
        break;
    case PeerState::Completed:
        // We only ever connect *because* the wake word armed a turn, so the
        // conversation is already active — leave conversation_active_ alone and
        // go straight to Listening. The capture task flushes the buffered
        // utterance now that connected_ is true.
        connected_          = true;
        last_rx_frame_tick_ = 0;
        last_rx_pkt_tick_   = xTaskGetTickCount();   // liveness baseline
        peer_dead_          = false;
        // Restart the turn clock at connect: bring-up may have eaten most of the
        // window the wake word set, and the user's speech (buffered during
        // bring-up) won't bump it again — so give the backend a full window from
        // here to deliver the first reply.
        turn_deadline_      = xTaskGetTickCount() + pdMS_TO_TICKS(kAwaitResponseMs);
        // Don't force a state here — the playback tick's resolveLedState picks
        // the right one next tick (Thinking if the user already asked during
        // bring-up, Listening if they only woke it). Forcing Listening caused a
        // one-tick green flash before it flipped to amber.
        fsm_.onEvent(domain::SessionEvent::PeerLive);
        break;
    case PeerState::Failed:
    case PeerState::Disconnected:
    case PeerState::Closed:
        // libpeer detected the path dropped. Flag it but DON'T end the turn
        // here — mainLoop decides whether to reconnect (mid-conversation) or
        // give up, the same way it handles a silent media death. Don't touch
        // peer_ from this callback: it runs inside peer_->tick().
        connected_ = false;
        peer_dead_ = true;
        reconnects_.fetch_add(1);
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
        ESP_LOGE(kTag, "signaling.sendOffer failed; abandoning this turn");
        conversation_active_ = false;   // mainLoop tears the half-built peer down
        return;
    }
    peer_->publishAnswer(std::move(resp->remote_sdp));
}

void Session::onInboundAudio(const uint8_t* data, std::size_t size)
{
    if (!data || size == 0 || !playback_buf_) return;

    // Liveness: any inbound packet (incl. silence keep-alive) proves the media
    // path is alive. mainLoop watches this to detect a dead path mid-session.
    last_rx_pkt_tick_ = xTaskGetTickCount();

    // Inbound is G.722: each octet decodes to two 16 kHz samples, ready for the
    // I2S DAC with no resampling. Cap the payload so 2× expansion can't
    // overflow pcm[]. The decoder is stateful (g722_dec_), reset per connection
    // in buildAndOffer().
    static int16_t pcm[domain::kMaxDecodedSamples];
    constexpr std::size_t kMaxBytes = (sizeof(pcm) / sizeof(pcm[0])) / 2;
    if (size > kMaxBytes) size = kMaxBytes;

    const std::size_t samples = domain::g722_decode(g722_dec_, data, size, pcm);  // = size*2

    const std::size_t sent = xStreamBufferSend(playback_buf_, pcm, samples * sizeof(int16_t), 0);
    const int32_t peak = domain::peak_abs_i16(pcm, static_cast<int>(samples));

    // Downlink visibility: count every inbound audio packet (regardless of
    // level) so /diag shows whether the backend's TTS is reaching us at all,
    // and the WS log shows it live (rate-limited).
    const uint32_t n = rx_audio_pkts_.fetch_add(1) + 1;
    rx_audio_last_peak_ = peak;
    if (peak > rx_audio_max_peak_.load()) rx_audio_max_peak_ = peak;
    if ((n % 100) == 1) {
        ESP_LOGI(kTag, "rx audio: pkt#%u bytes=%u peak=%ld queued=%u/%u",
                 (unsigned)n, (unsigned)size, (long)peak,
                 (unsigned)sent, (unsigned)(samples * sizeof(int16_t)));
    }

    if (peak >= kSpeakingPcmThreshold) {
        const TickType_t now = xTaskGetTickCount();
        last_rx_frame_tick_ = now;
        bot_replied_ = true;   // first reply landed: switch to post-reply timing
        // The bot is answering: keep the turn open, and start the (short)
        // post-reply silence countdown from this frame. Each frame pushes it,
        // so it only elapses once the reply has actually stopped.
        turn_deadline_ = now + pdMS_TO_TICKS(kPostResponseSilenceMs);
    }
}


void Session::mainLoopTaskEntry(void* arg) { static_cast<Session*>(arg)->mainLoopTask(); }
void Session::captureTaskEntry (void* arg) { static_cast<Session*>(arg)->captureTask(); }
void Session::playbackTaskEntry(void* arg) { static_cast<Session*>(arg)->playbackTask(); }

void Session::mainLoopTask()
{
    TickType_t connect_started = 0;
    bool       prev_want       = false;
    int        reconnects      = 0;   // mid-talk reconnects used this turn

    while (running_.load()) {
        const TickType_t now  = xTaskGetTickCount();
        const bool       want = conversation_active_.load();
        const bool       have = (peer_ != nullptr);
        if (want && !prev_want) reconnects = 0;   // a fresh turn resets the budget
        prev_want = want;

        if (want && !have) {
            // Bring up a session — a fresh wake, or a rebuild after a mid-talk
            // drop. The capture task is already buffering the user's speech into
            // the backlog ring, so nothing spoken during bring-up is lost.
            ESP_LOGI(kTag, "%s", reconnects ? "reconnecting" : "wake → connecting");
            connect_started = now;
            peer_dead_      = false;
            if (!buildAndOffer()) {
                ESP_LOGE(kTag, "buildAndOffer failed; abandoning turn");
                conversation_active_ = false;
                std::lock_guard<std::mutex> lk(peer_mtx_);
                peer_.reset();
            }
        } else if (!want && have) {
            // Conversation ended (or we've given up) → tear the session down and
            // go idle. The backend sees the peer drop and reaps its pipeline;
            // the next wake word starts clean. The lock + connected_=false here
            // pair with the capture task's send guard so we never destroy peer_
            // out from under an in-flight sendAudio.
            ESP_LOGI(kTag, "conversation ended → disconnecting");
            {
                std::lock_guard<std::mutex> lk(peer_mtx_);
                connected_ = false;
                peer_.reset();
            }
        } else if (want && have) {
            // A turn is live. Detect a dropped connection two ways: libpeer
            // flagged it (peer_dead_, e.g. ICE consent lost), or — the silent
            // case where consent survives but media stopped — no inbound audio
            // for kMediaDeadMs while connected. Either way reconnect (the user
            // is mid-talk), up to a cap, then give up and end the session.
            const bool media_dead =
                connected_.load() && (now - last_rx_pkt_tick_.load()) > pdMS_TO_TICKS(kMediaDeadMs);
            if (peer_dead_.load() || media_dead) {
                if (reconnects < kMaxReconnectsPerTurn) {
                    ++reconnects;
                    ESP_LOGW(kTag, "connection lost mid-talk (%s) → reconnect %d/%d",
                             peer_dead_.load() ? "peer" : "media", reconnects, kMaxReconnectsPerTurn);
                    std::lock_guard<std::mutex> lk(peer_mtx_);
                    connected_ = false;
                    peer_.reset();   // next iteration rebuilds (want && !have)
                } else {
                    ESP_LOGW(kTag, "connection lost; reconnects exhausted → ending session");
                    conversation_active_ = false;
                }
            } else if (!connected_.load() &&
                       (now - connect_started) > pdMS_TO_TICKS(kConnectTimeoutMs)) {
                // Still negotiating and stalled → abandon the turn.
                ESP_LOGW(kTag, "connect timed out; abandoning turn");
                conversation_active_ = false;
            }
        }

        if (peer_) peer_->tick();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(nullptr);
}

bool Session::nextEvent(std::string& out)
{
    std::lock_guard<std::mutex> lk(events_mtx_);
    if (events_.empty()) return false;
    out = std::move(events_.front());
    events_.pop_front();
    return true;
}

// ---------- Capture task (CoreS3 mono mic) ---------------------------------

void Session::captureTask()
{
    static int16_t raw[domain::kFramesPerPacket];
    static int16_t mono_wake[domain::kFramesPerPacket];
    static int16_t mono_uplink[domain::kFramesPerPacket];
    static uint8_t wire_buf[domain::kFramesPerPacket / 2];
    static const int16_t zero_pcm[domain::kFramesPerPacket] = {0};

    // Uplink backlog: speech during WebRTC bring-up is buffered and flushed
    // once connected (≈ 8 s), in PSRAM.
    const std::size_t kPktBytes = domain::kFramesPerPacket / 2;
    const std::size_t kRingPkts = 400;
    uint8_t* ring = static_cast<uint8_t*>(heap_caps_malloc(kRingPkts * kPktBytes, MALLOC_CAP_SPIRAM));
    std::size_t r_head = 0, r_count = 0;
    auto ring_push = [&](const uint8_t* p) {
        if (!ring) return;
        const std::size_t idx = (r_head + r_count) % kRingPkts;
        std::memcpy(ring + idx * kPktBytes, p, kPktBytes);
        if (r_count < kRingPkts) r_count++;
        else r_head = (r_head + 1) % kRingPkts;
    };
    auto ring_pop = [&](uint8_t* out) -> bool {
        if (!ring || r_count == 0) return false;
        std::memcpy(out, ring + r_head * kPktBytes, kPktBytes);
        r_head = (r_head + 1) % kRingPkts;
        r_count--;
        return true;
    };

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
        const bool bot_speaking = rx != 0 && (now - rx) < pdMS_TO_TICKS(kEchoGuardMs);

        const uint32_t rms = domain::rms_i16(mono_wake, domain::kFramesPerPacket);
        mic_level_ = std::min(1.0f, rms / 8000.0f);
        if (!bot_speaking && rms >= static_cast<uint32_t>(kMicActiveRmsThreshold)) {
            last_mic_active_tick_ = now;
            if (conversation_active_.load() && !bot_replied_.load())
                turn_deadline_ = now + pdMS_TO_TICKS(kAwaitResponseMs);
        }

        transport::WakeEngine::process(mono_wake, domain::kFramesPerPacket);
        const bool woke = transport::WakeEngine::detected() || ptt_.exchange(false);
        if (woke) {
            if (!conversation_active_.exchange(true)) {
                ESP_LOGI(kTag, "wake → turn armed");
                domain::g722_init(g722_enc_);
                r_head = r_count = 0;
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

        const int16_t* src = bot_speaking ? zero_pcm : mono_uplink;
        domain::g722_encode(g722_enc_, src, domain::kFramesPerPacket, wire_buf);
        ring_push(wire_buf);

        if (connected_.load()) {
            std::lock_guard<std::mutex> lk(peer_mtx_);
            if (peer_) {
                uint8_t pkt[domain::kFramesPerPacket / 2];
                int budget = (r_count > 1) ? 3 : 1;
                while (budget-- > 0 && ring_pop(pkt)) peer_->sendAudio(pkt, sizeof pkt);
            }
        }
    }
    if (ring) heap_caps_free(ring);
    vTaskDelete(nullptr);
}

// ---------- Playback task ---------------------------------------------------

void Session::playbackTask()
{
    static int16_t mono[domain::kFramesPerPacket];
    int16_t* chirp = static_cast<int16_t*>(heap_caps_malloc(domain::kChirpMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));

    while (running_.load()) {
        const int ch = chirp_pending_.exchange(-1);
        if (ch >= 0 && chirp) {
            const std::size_t cn = domain::synth_chirp(static_cast<domain::Chirp>(ch), chirp, domain::kChirpMaxSamples);
            audio_.write(chirp, cn);
        }
        std::size_t got = playback_buf_ ? xStreamBufferReceive(playback_buf_, mono, sizeof(mono), pdMS_TO_TICKS(50)) : 0;
        std::size_t frames = got / sizeof(int16_t);
        if (frames < domain::kFramesPerPacket) {
            std::memset(mono + frames, 0, sizeof(mono) - frames * sizeof(int16_t));
        }
        // Mouth envelope: fast attack, slower release.
        const float peak = domain::peak_abs_i16(mono, domain::kFramesPerPacket) / 12000.0f;
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
