#include "app/session.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "domain/energy_gate.hpp"
#include "domain/g722.hpp"
#include "domain/gain.hpp"
#include "hal/https_client.hpp"
#include "transport/wake_engine.hpp"

namespace {

constexpr const char* kTag = "session";

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

// Wrap mono 16-bit PCM in a minimal 44-byte WAV container, so the captured
// wake audio downloads/uploads as a self-contained .wav.
std::string makeWav(const int16_t* pcm, std::size_t n, uint32_t sr)
{
    const uint32_t data_bytes = static_cast<uint32_t>(n * 2);
    std::string w;
    w.reserve(44 + data_bytes);
    auto u32 = [&](uint32_t v) { for (int i = 0; i < 4; ++i) w.push_back(char((v >> (8 * i)) & 0xff)); };
    auto u16 = [&](uint16_t v) { for (int i = 0; i < 2; ++i) w.push_back(char((v >> (8 * i)) & 0xff)); };
    w += "RIFF"; u32(36 + data_bytes); w += "WAVE";
    w += "fmt "; u32(16); u16(1); u16(1); u32(sr); u32(sr * 2); u16(2); u16(16);
    w += "data"; u32(data_bytes);
    w.append(reinterpret_cast<const char*>(pcm), data_bytes);
    return w;
}

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
// Wake-sample uploader: low priority, off the AV core, with enough stack for an
// mbedTLS POST. Best-effort background work — must never disturb audio.
constexpr int  kUploadStack           = 8 * 1024;
constexpr int  kUploadPrio            = 4;

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
    if (wake_pcm_)     heap_caps_free(wake_pcm_);
}

bool Session::getWakeSample(std::string& wav, std::string& meta, uint32_t& seq)
{
    std::lock_guard<std::mutex> lk(wake_mtx_);
    if (!wake_pcm_ || wake_pcm_len_ == 0) return false;
    wav = makeWav(wake_pcm_, wake_pcm_len_, kWakeSampleRate);
    const auto& m = wake_metrics_;
    char buf[480];
    int len = std::snprintf(buf, sizeof buf,
        "{\"fire_seq\":%u,\"peak\":%.3f,\"avg\":%.3f,\"hits\":%d,"
        "\"window\":[%.3f,%.3f,%.3f,%.3f,%.3f],"
        "\"sample_rate\":%d,\"gain_db\":12,\"samples\":%u,\"uptime_ms\":%u}",
        (unsigned)m.fire_seq, (double)m.peak, (double)m.avg, m.hits,
        (double)m.window[0], (double)m.window[1], (double)m.window[2],
        (double)m.window[3], (double)m.window[4],
        kWakeSampleRate, (unsigned)wake_pcm_len_, (unsigned)esp_log_timestamp());
    meta.assign(buf, len > 0 ? static_cast<std::size_t>(len) : 0);
    seq = wake_capture_seq_.load();
    return true;
}

bool Session::getWakeSampleUpload(std::string& wav, std::string& query, uint32_t& seq)
{
    std::lock_guard<std::mutex> lk(wake_mtx_);
    if (!wake_pcm_ || wake_pcm_len_ == 0) return false;
    wav = makeWav(wake_pcm_, wake_pcm_len_, kWakeSampleRate);
    const auto& m = wake_metrics_;
    char buf[400];
    int len = std::snprintf(buf, sizeof buf,
        "seq=%u&peak=%.3f&avg=%.3f&hits=%d&win=%.3f,%.3f,%.3f,%.3f,%.3f"
        "&sr=%d&samples=%u&uptime=%u",
        (unsigned)m.fire_seq, (double)m.peak, (double)m.avg, m.hits,
        (double)m.window[0], (double)m.window[1], (double)m.window[2],
        (double)m.window[3], (double)m.window[4],
        kWakeSampleRate, (unsigned)wake_pcm_len_, (unsigned)esp_log_timestamp());
    query.assign(buf, len > 0 ? static_cast<std::size_t>(len) : 0);
    seq = wake_capture_seq_.load();
    return true;
}

void Session::uploadTaskEntry(void* arg) { static_cast<Session*>(arg)->uploadTask(); }

void Session::uploadTask()
{
    // Backend base + "/wake-sample?" once; query is appended per upload.
    std::string base = backend_url_;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const std::string endpoint = base + "/wake-sample?";
    hal::HttpsClient http{1024};   // ack body is tiny JSON

    while (running_.load()) {
        // Wake on a snapshot notification; a short timeout lets us re-check
        // running_ for a prompt shutdown.
        if (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(200)) == 0) continue;

        std::string wav, query; uint32_t seq = 0;
        if (!getWakeSampleUpload(wav, query, seq)) continue;

        const std::string url = endpoint + query;
        hal::HttpResponse resp;
        esp_err_t err = http.request(url.c_str(), HTTP_METHOD_POST, wav, "audio/wav", resp);
        if (err != ESP_OK)
            ESP_LOGW(kTag, "wake-sample upload transport err: %s", esp_err_to_name(err));
        else if (resp.status / 100 != 2)
            ESP_LOGW(kTag, "wake-sample upload status=%d", resp.status);
        else
            ESP_LOGI(kTag, "wake-sample uploaded seq=%u (%u B)",
                     (unsigned)seq, (unsigned)wav.size());
    }
    vTaskDelete(nullptr);
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

    // PSRAM snapshot buffer for the audio that triggers a wake (filled by
    // captureTask on each fire). Non-fatal if it fails — just disables capture.
    wake_pcm_ = static_cast<int16_t*>(
        heap_caps_malloc(kWakeCaptureSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!wake_pcm_) ESP_LOGW(kTag, "wake-capture buffer alloc failed; /wake.wav disabled");
    // G.722 codec state (g722_enc_/g722_dec_) is reset per connection in
    // buildAndOffer(); nothing to allocate here.

    transport::WakeEngine::initOnce();

    // On-demand connection model: we do NOT open a WebRTC session at startup.
    // The wake word runs locally on the mic path and needs no backend, so the
    // device sits dark and idle until it fires. mainLoop then builds a session,
    // and tears it down when the conversation ends — no persistent connection
    // to keep alive, no backend idle timeout to dodge, no reconnect to manage.
    ui_.setLed(domain::LedState::Off);

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
    // Wake-sample uploader is best-effort: if it can't start, capture + the
    // local /wake.wav endpoint still work, we just don't push to the backend.
    if (xTaskCreatePinnedToCore(uploadTaskEntry, "wake_up", kUploadStack, this,
                                kUploadPrio, &t_upload_, kMainCore) != pdPASS) {
        ESP_LOGW(kTag, "wake-sample uploader task create failed; uploads disabled");
        t_upload_ = nullptr;
    }
}

void Session::stop()
{
    if (!running_.exchange(false)) return;
    // Tasks see running_=false and exit; give them a tick to wind down,
    // then null the handles. (vTaskDelete from another task races with
    // a self-exit; we prefer the self-exit path.)
    vTaskDelay(pdMS_TO_TICKS(50));
    t_main_ = t_cap_ = t_play_ = t_upload_ = nullptr;
    peer_.reset();
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
        ui_.setLed(domain::LedState::Negotiating);
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

// ---------- Main loop task -----------------------------------------------

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
            ui_.setLed(domain::LedState::Off);
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
                    ui_.setLed(domain::LedState::Negotiating);
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
    static uint8_t wire_buf   [domain::kFramesPerPacket / 2];    // 160 B G.722 on the wire
    // A zero PCM frame, G.722-encoded and sent when the turn is idle/echo-
    // guarded. This keeps the RTP stream — and thus the backend pipeline —
    // alive without the backend's VAD hearing anything; without it the backend
    // idle-times-out and tears down the WebRTC session during silence. We run
    // zeros through the *same* encoder (not a constant byte pattern) so its
    // adaptive state stays coherent across silence→speech transitions.
    static const int16_t zero_pcm[domain::kFramesPerPacket] = {0};

    // Uplink backlog ring. While a wake-triggered connection is still being set
    // up (~1-3 s of relay/ICE/DTLS), the user is already talking. We encode and
    // buffer those frames here, then flush them once connected so the start of
    // the question isn't lost. 400 packets ≈ 8 s — well over worst-case
    // bring-up. Internal RAM is tight on this board, so it lives in PSRAM.
    const std::size_t kPktBytes = domain::kFramesPerPacket / 2;   // 160 B / 20 ms
    const std::size_t kRingPkts = 400;
    uint8_t* ring = static_cast<uint8_t*>(
        heap_caps_malloc(kRingPkts * kPktBytes, MALLOC_CAP_SPIRAM));
    if (!ring) ESP_LOGW(kTag, "uplink backlog alloc failed; first words may clip");
    std::size_t r_head = 0, r_count = 0;
    auto ring_push = [&](const uint8_t* p) {
        if (!ring) return;
        const std::size_t idx = (r_head + r_count) % kRingPkts;
        std::memcpy(ring + idx * kPktBytes, p, kPktBytes);
        if (r_count < kRingPkts) r_count++;
        else r_head = (r_head + 1) % kRingPkts;   // full: drop oldest frame
    };
    auto ring_pop = [&](uint8_t* out) -> bool {
        if (!ring || r_count == 0) return false;
        std::memcpy(out, ring + r_head * kPktBytes, kPktBytes);
        r_head = (r_head + 1) % kRingPkts;
        r_count--;
        return true;
    };

    // Rolling buffer of recent mic audio (mono_uplink — undistorted +12 dB) so
    // that when the wake word fires we can snapshot the ~2 s that triggered it.
    // A sample ring (not packet ring) so the snapshot is a contiguous WAV.
    int16_t* cap_ring = static_cast<int16_t*>(
        heap_caps_malloc(kWakeCaptureSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!cap_ring) ESP_LOGW(kTag, "wake-capture ring alloc failed; samples won't be recorded");
    std::size_t c_head = 0, c_count = 0;
    auto cap_push = [&](const int16_t* p, std::size_t n) {
        if (!cap_ring) return;
        for (std::size_t i = 0; i < n; ++i) {
            cap_ring[(c_head + c_count) % kWakeCaptureSamples] = p[i];
            if (c_count < kWakeCaptureSamples) c_count++;
            else c_head = (c_head + 1) % kWakeCaptureSamples;
        }
    };

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
        cap_push(mono_uplink, domain::kFramesPerPacket);   // roll the wake-capture buffer

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
            last_mic_active_tick_ = now;   // drives the Talking LED
            // Extend the turn on user speech ONLY while still awaiting the bot's
            // first reply. Once it has answered, the post-reply silence window
            // (set per inbound TTS frame) governs end-of-turn, so ambient room
            // noise can't keep re-arming the deadline and hold the session open
            // long after the answer — the device hangs up shortly after the
            // reply, and a follow-up just needs another wake word.
            if (conversation_active_.load() && !bot_replied_.load())
                turn_deadline_ = now + pdMS_TO_TICKS(kAwaitResponseMs);
        }

        // Wake word arms the conversation (starts streaming to the backend).
        // Note: do NOT bump last_mic_active_tick_ here — that would make the LED
        // read "Talking" right after the flash; we want "Listening" (go ahead).
        transport::WakeEngine::process(mono_wake, domain::kFramesPerPacket);
        if (transport::WakeEngine::detected()) {
            // Snapshot the audio that triggered this fire + the metrics that
            // fired it, for hard-negative collection. Two memcpys (the ring may
            // wrap) under the lock; cheap and infrequent (≥2 s cooldown).
            if (cap_ring && wake_pcm_) {
                std::lock_guard<std::mutex> lk(wake_mtx_);
                const std::size_t n     = c_count;
                const std::size_t first = std::min(n, kWakeCaptureSamples - c_head);
                std::memcpy(wake_pcm_, cap_ring + c_head, first * sizeof(int16_t));
                if (n > first)
                    std::memcpy(wake_pcm_ + first, cap_ring, (n - first) * sizeof(int16_t));
                wake_pcm_len_ = n;
                wake_metrics_ = transport::WakeEngine::lastMetrics();
                wake_capture_seq_.fetch_add(1);
                if (t_upload_) xTaskNotifyGive(t_upload_);   // kick the uploader
            }
            // Arm a turn. On the idle→active edge, start this utterance fresh:
            // a clean encoder + empty backlog, and flash the wake ack. mainLoop
            // sees conversation_active_ and brings the session up.
            if (!conversation_active_.exchange(true)) {
                ui_.setLed(domain::LedState::WakeAck);
                domain::g722_init(g722_enc_);
                r_head = r_count = 0;
                bot_replied_ = false;   // awaiting this turn's first reply
                chirp_pending_ = static_cast<int>(domain::Chirp::Wake);  // "online" blip
            }
            turn_deadline_ = now + pdMS_TO_TICKS(kAwaitResponseMs);
        }

        // End the turn only once the deadline lapses. The deadline is pushed by
        // user speech (above, await regime) and by each inbound bot TTS frame
        // (onInboundAudio, post-reply regime) — so the short post-reply silence
        // window can't elapse until the bot has actually finished answering.
        if (conversation_active_.load()) {
            if (now > turn_deadline_.load()) {
                conversation_active_ = false;
                chirp_pending_ = static_cast<int>(domain::Chirp::End);  // "offline" blip
                ESP_LOGI(kTag, "conversation idle — wake word required again");
            }
        }

        // Outside a turn we hold no connection — keep running the local wake
        // word (above) and send nothing.
        if (!conversation_active_.load()) continue;

        // In a turn: encode the user's mic (or encoded silence while the bot
        // speaks / we're muted — echo guard) through the one stateful encoder,
        // and append to the backlog ring.
        const bool send_mic = !bot_speaking && !button_.isMuted();
        const int16_t* src  = send_mic ? mono_uplink : zero_pcm;
        domain::g722_encode(g722_enc_, src, domain::kFramesPerPacket, wire_buf);
        ring_push(wire_buf);

        // Flush only once the session is up. Drain with a mild catch-up (up to
        // 3 packets / 20 ms) so the buffered head-start reaches the backend
        // promptly without arriving as one burst the jitter buffer would drop.
        // The lock + peer_ re-check pair with mainLoop's teardown so we never
        // sendAudio on a peer_ being destroyed on the other core.
        if (connected_.load()) {
            std::lock_guard<std::mutex> lk(peer_mtx_);
            if (peer_) {
                if (ring) {
                    uint8_t pkt[domain::kFramesPerPacket / 2];
                    int budget = (r_count > 1) ? 3 : 1;
                    while (budget-- > 0 && ring_pop(pkt))
                        peer_->sendAudio(pkt, sizeof pkt);          // 160 B / packet
                } else {
                    peer_->sendAudio(wire_buf, sizeof wire_buf);
                }
            }
        }
        // else: still negotiating — leave it buffered; mainLoop is connecting.
    }
    if (ring) heap_caps_free(ring);
    if (cap_ring) heap_caps_free(cap_ring);
    vTaskDelete(nullptr);
}

// ---------- Playback task -------------------------------------------------

void Session::playbackTask()
{
    static int16_t mono  [domain::kFramesPerPacket];
    static int32_t stereo[domain::kFramesPerPacket * hal::AudioIo::kChannels];
    // Chirp scratch lives in PSRAM — at ~28 KB it would crowd the scarce
    // internal RAM. Allocated once for the task's lifetime.
    int16_t* chirp = static_cast<int16_t*>(
        heap_caps_malloc(domain::kChirpMaxSamples * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    if (!chirp) ESP_LOGW(kTag, "chirp buffer alloc failed; UI chirps disabled");

    while (running_.load()) {
        // Local UI chirp (wake / end-of-session). Synthesised and played here,
        // the sole I2S writer, so it can't race the TTS stream on the speaker.
        // It briefly pre-empts playback — fine, since wake fires before any TTS
        // and end fires after it has stopped.
        const int ch = chirp_pending_.exchange(-1);
        if (ch >= 0 && chirp) {
            const std::size_t cn =
                domain::synth_chirp(static_cast<domain::Chirp>(ch), chirp, domain::kChirpMaxSamples);
            for (std::size_t off = 0; off < cn; off += domain::kFramesPerPacket) {
                const std::size_t f = std::min(domain::kFramesPerPacket, cn - off);
                for (std::size_t i = 0; i < f; ++i) {
                    const int32_t s = static_cast<int32_t>(chirp[off + i]) << 16;
                    stereo[i * hal::AudioIo::kChannels + kAecRefTxChannel]       = s;
                    stereo[i * hal::AudioIo::kChannels + (1 - kAecRefTxChannel)] = 0;
                }
                audio_.write(stereo, f);
            }
        }

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
    if (chirp) heap_caps_free(chirp);
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

    char buf[768];
    snprintf(buf, sizeof buf,
        "{\"uptime_s\":%lld,"
        "\"heap_internal_free\":%u,\"heap_internal_min\":%u,\"psram_free\":%u,"
        "\"connected\":%s,\"peer_state\":\"%s\",\"reconnects\":%u,"
        "\"conversation_active\":%s,\"muted\":%s,"
        "\"ms_since_tts\":%ld,\"ms_since_mic\":%ld,\"wake_p\":%.3f,"
        "\"rx_audio_pkts\":%u,\"rx_audio_peak\":%ld,\"rx_audio_max_peak\":%ld,"
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
        static_cast<unsigned>(rx_audio_pkts_.load()),
        static_cast<long>(rx_audio_last_peak_.load()),
        static_cast<long>(rx_audio_max_peak_.load()),
        stack_free(t_main_), stack_free(t_cap_), stack_free(t_play_));
    return std::string(buf);
}

} // namespace app
