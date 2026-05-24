#include "webrtc_session.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "opus.h"

#include "audio_io.h"
#include "button.h"
#include "leds.h"
#include "pipecat_signaling.h"
#include "wake_word.h"

static const char *TAG = "webrtc";

// DTLS handshake + ICE state machine recurses deep — 4 KB is not enough,
// hits the stack guard during the SRTP handshake. peer_demo example uses
// 10 KB; matching that here.
#define MAIN_LOOP_TASK_STACK   10240
#define CAPTURE_TASK_STACK     8192
#define PLAYBACK_TASK_STACK    8192
#define FRAMES_PER_PKT         320     // 20 ms @ 16 kHz — matches Opus VoIP framing
#define PCM_BYTES_PER_PKT      (FRAMES_PER_PKT * sizeof(int16_t))
#define OPUS_MAX_PACKET_BYTES  600     // worst-case 20 ms @ 32 kbps + headroom
#define OPUS_BITRATE_BPS       24000   // 24 kbps mono voice — comfortable margin
#define OPUS_COMPLEXITY        5       // 0..10; 5 is the practical S3 sweet spot

// Playback jitter buffer: 200 ms of mono int16 @ 16 kHz. Long enough to
// absorb network bursts, short enough that barge-in feels responsive.
#define PLAYBACK_BUFFER_BYTES  (16000 * 2 / 5)
// Mark as "speaking" if a TTS frame arrived within this window.
#define SPEAKING_HOLD_MS       400
// End the conversation if no wake event AND no inbound TTS for this long.
#define SESSION_IDLE_TIMEOUT_MS  10000

// Channel mapping of TTS playback inside the stereo I2S TX frame to XVF3800.
// XMOS docs say AEC reference goes on the **left** channel of XVF3800's I2S
// input; we keep this as a #define so flipping it on the bench is one line.
#ifndef AEC_REF_TX_CHANNEL
#define AEC_REF_TX_CHANNEL  0    // 0 = L, 1 = R
#endif

typedef struct {
    esp_peer_handle_t           peer;
    pipecat_signaling_t        *sig;
    // Ownership of the ICE-server backing strings — esp_peer keeps pointers
    // into this list and uses them throughout the session, so it must
    // outlive the peer handle. Freed in webrtc_session_stop.
    pipecat_ice_server_t       *ice_servers;
    size_t                      ice_server_count;
    esp_peer_ice_server_cfg_t  *ice_server_cfgs;
    OpusEncoder           *opus_enc;
    OpusDecoder           *opus_dec;
    StreamBufferHandle_t   playback_buf;
    TaskHandle_t           main_loop_task;
    TaskHandle_t           capture_task;
    TaskHandle_t           playback_task;
    volatile bool          running;
    volatile bool          connected;
    volatile bool          conv_active;     // currently in a wake-triggered conversation
    volatile TickType_t    last_rx_frame_tick;
    volatile TickType_t    last_activity_tick;  // wake event or inbound TTS
    // 0 = no retry pending; nonzero = tick at which main_loop_task should
    // call esp_peer_new_connection again. Set when on_state reports
    // CONNECT_FAILED / DISCONNECTED so a backend hiccup or ESP32_COMPAT
    // flip recovers without rebooting the device.
    volatile TickType_t    retry_at_tick;
    uint32_t               pts;        // monotonic frame timestamp
} session_t;

#define RETRY_INTERVAL_MS   5000

static session_t s_session = {0};

// ---------- esp_peer callbacks --------------------------------------------

static int on_state(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "peer state = %d", (int)state);
    // esp_peer 1.4 enum (from include/esp_peer.h):
    //   0 CLOSED   1 DISCONNECTED   2 NEW_CONNECTION   3 CANDIDATE_GATHERING
    //   4 PAIRING  5 PAIRED         6 CONNECTING       7 CONNECTED
    //   8 CONNECT_FAILED            9 DATA_CHANNEL_CONNECTED  ...
    // Media (audio frames) can flow once CONNECTED (7) — that's when DTLS-
    // SRTP is up. PAIRED (5) means ICE pair selected but DTLS still
    // handshaking; CONNECTED is the right "OK to send audio" trigger.
    switch (state) {
    case ESP_PEER_STATE_NEW_CONNECTION:
    case ESP_PEER_STATE_CANDIDATE_GATHERING:
    case ESP_PEER_STATE_PAIRING:
    case ESP_PEER_STATE_PAIRED:
    case ESP_PEER_STATE_CONNECTING:
        // Negotiation in progress — amber spin so the user can tell at a
        // glance that signaling worked but ICE/DTLS hasn't completed yet.
        leds_set(LED_STATE_NEGOTIATING);
        break;
    case ESP_PEER_STATE_CONNECTED:
    case ESP_PEER_STATE_DATA_CHANNEL_CONNECTED:
        if (!s_session.connected) {
            ESP_LOGI(TAG, "session ready for media");
        }
        s_session.connected = true;
        leds_set(LED_STATE_LISTENING);
        break;
    case ESP_PEER_STATE_DISCONNECTED:
    case ESP_PEER_STATE_CLOSED:
    case ESP_PEER_STATE_CONNECT_FAILED:
        s_session.connected = false;
        s_session.retry_at_tick =
            xTaskGetTickCount() + pdMS_TO_TICKS(RETRY_INTERVAL_MS);
        leds_set(LED_STATE_CONNECTING);
        break;
    default:
        break;
    }
    return 0;
}

// Inbound TTS audio from the backend. Decode Opus → mono int16 → push to
// the jitter buffer. The dedicated playback task drains it to I2S.
static int on_audio_data(esp_peer_audio_frame_t *frame, void *ctx)
{
    if (!frame || !frame->data || !s_session.opus_dec) return -1;

    static int16_t pcm[FRAMES_PER_PKT * 6];   // headroom for any well-formed Opus packet
    int decoded = opus_decode(s_session.opus_dec,
                              frame->data, frame->size,
                              pcm, sizeof(pcm) / sizeof(pcm[0]),
                              /*decode_fec=*/0);
    if (decoded <= 0) {
        ESP_LOGW(TAG, "opus_decode err %d", decoded);
        return -1;
    }
    size_t bytes = (size_t)decoded * sizeof(int16_t);
    size_t sent  = xStreamBufferSend(s_session.playback_buf, pcm, bytes, 0);
    if (sent < bytes) {
        // Buffer full → drop the oldest by resetting. Network bursts can
        // outpace I2S draining briefly; a clean drop avoids stair-stepping
        // latency upward forever.
        ESP_LOGW(TAG, "playback buffer full, dropped %u bytes", (unsigned)(bytes - sent));
    }
    s_session.last_rx_frame_tick = xTaskGetTickCount();
    s_session.last_activity_tick = s_session.last_rx_frame_tick;
    return 0;
}

// esp_peer hands us outbound signaling messages here: either the local SDP
// offer (after esp_peer_new_connection) or trickled ICE candidates.
static int on_msg(esp_peer_msg_t *msg, void *ctx)
{
    if (!msg || !msg->data) return -1;

    switch (msg->type) {
    case ESP_PEER_MSG_TYPE_SDP: {
        // SDP offer to ship to the backend. Backend returns the SDP answer,
        // which we hand back to esp_peer via esp_peer_send_msg.
        char *answer_sdp = NULL;
        esp_err_t err = pipecat_signaling_send_offer(
            s_session.sig, (const char *)msg->data, &answer_sdp);
        if (err != ESP_OK || !answer_sdp) {
            ESP_LOGE(TAG, "send_offer failed: %s", esp_err_to_name(err));
            return -1;
        }
        esp_peer_msg_t answer = {
            .type = ESP_PEER_MSG_TYPE_SDP,
            .data = (uint8_t *)answer_sdp,
            .size = strlen(answer_sdp) + 1,
        };
        int r = esp_peer_send_msg(s_session.peer, &answer);
        free(answer_sdp);
        return r;
    }
    case ESP_PEER_MSG_TYPE_CANDIDATE: {
        // Single trickled ICE candidate. esp_peer hands it to us as a single
        // SDP "candidate:" line — we forward verbatim. sdp_mid + mline index
        // aren't separately passed by this API revision; the backend's
        // SmallWebRTC handler accepts the common case (mid="0", mline_index=0).
        pipecat_ice_candidate_t c = {
            .candidate       = (const char *)msg->data,
            .sdp_mid         = "0",
            .sdp_mline_index = 0,
        };
        if (pipecat_signaling_send_ice(s_session.sig, &c, 1) != ESP_OK) {
            ESP_LOGW(TAG, "ice forward failed (non-fatal)");
        }
        return 0;
    }
    default:
        ESP_LOGW(TAG, "unhandled msg type %d", (int)msg->type);
        return 0;
    }
}

// ---------- Worker tasks --------------------------------------------------

static void main_loop_task(void *arg)
{
    while (s_session.running) {
        if (s_session.peer) esp_peer_main_loop(s_session.peer);
        // Recover from a failed/dropped peer connection (most common cause:
        // backend just redeployed, ESP32_COMPAT toggled, transient TLS
        // hiccup). on_state armed retry_at_tick when the failure fired;
        // we re-arm new_connection here once the cooldown elapsed.
        if (s_session.peer && s_session.retry_at_tick &&
            xTaskGetTickCount() >= s_session.retry_at_tick) {
            ESP_LOGI(TAG, "retrying peer connection");
            s_session.retry_at_tick = 0;
            esp_peer_new_connection(s_session.peer);
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

static void capture_task(void *arg)
{
    // Reads stereo 32-bit frames from XVF3800, downmixes to mono int16,
    // always feeds the wake-word detector, and (only when in an active
    // wake-triggered conversation) Opus-encodes packets for the backend.
    static int32_t stereo[FRAMES_PER_PKT * AUDIO_IO_CHANNELS];
    static int16_t mono[FRAMES_PER_PKT];
    static uint8_t opus_buf[OPUS_MAX_PACKET_BYTES];

    while (s_session.running) {
        esp_err_t rd = audio_io_read(stereo, FRAMES_PER_PKT);
        if (rd != ESP_OK) {
            // Don't tight-loop on partial reads / I2S stalls (XVF3800 clock
            // briefly pauses on config writes); 20 ms keeps us roughly aligned
            // with the next DMA descriptor.
            ESP_LOGW(TAG, "audio_io_read: %s", esp_err_to_name(rd));
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        // Channel 0 of XVF3800 = processed mono audio. 32-bit MSB-aligned;
        // XMOS effectively outputs only the upper ~16-19 bits at typical
        // mic levels, so a straight >>16 shift gives very quiet int16
        // (peaks ~5% of dynamic range during speech). Shift fewer bits +
        // saturate to bring voice up into the 30-50% range microWakeWord
        // expects. >>13 is a 3-bit (18 dB) software boost.
        for (size_t i = 0; i < FRAMES_PER_PKT; ++i) {
            int32_t s = stereo[i * AUDIO_IO_CHANNELS] >> 13;
            if (s >  INT16_MAX) s = INT16_MAX;
            if (s <  INT16_MIN) s = INT16_MIN;
            mono[i] = (int16_t)s;
        }

        // Always feed the wake-word detector — needs to run even before
        // WebRTC is ready (so "Эй, Фемто!" said during the connect window
        // queues the session to open as soon as media is up) and even with
        // the mic muted (user can still wake the device with a hardware
        // mute switch engaged).
        wake_word_process(mono, FRAMES_PER_PKT);
        if (wake_word_detected()) {
            if (!s_session.conv_active) {
                ESP_LOGI(TAG, "wake word — opening conversation");
            }
            s_session.conv_active       = true;
            s_session.last_activity_tick = xTaskGetTickCount();
            // Immediate visual feedback so the user knows the device heard
            // them before the backend round-trip starts producing TTS audio.
            leds_set(LED_STATE_WAKE_ACK);
        }

        // Conversation ends after idle timeout (no wake + no inbound TTS).
        if (s_session.conv_active &&
            (xTaskGetTickCount() - s_session.last_activity_tick) >
                pdMS_TO_TICKS(SESSION_IDLE_TIMEOUT_MS)) {
            ESP_LOGI(TAG, "conversation idle, closing");
            s_session.conv_active = false;
        }

        if (!s_session.connected ||
            !s_session.conv_active ||
            button_is_muted()) {
            // Not paired yet, pre-wake, or muted: do not transmit but keep
            // the wake detector hot so the next utterance can open / re-open
            // the conversation.
            continue;
        }

        int n = opus_encode(s_session.opus_enc, mono, FRAMES_PER_PKT,
                            opus_buf, sizeof(opus_buf));
        if (n < 0) {
            ESP_LOGW(TAG, "opus_encode err %d", n);
            continue;
        }
        esp_peer_audio_frame_t f = {
            .pts  = s_session.pts,
            .data = opus_buf,
            .size = n,
        };
        if (esp_peer_send_audio(s_session.peer, &f) != 0) {
            ESP_LOGW(TAG, "send_audio dropped");
        }
        s_session.pts += FRAMES_PER_PKT;  // 16 kHz sample-count clock
    }
    vTaskDelete(NULL);
}

// Drains the decoded-PCM jitter buffer into I2S TX. Always writes — when no
// TTS audio is pending, writes silence so the I2S clock stays alive (and so
// XVF3800's AEC reference channel sees a defined zero rather than DMA
// garbage).
static void playback_task(void *arg)
{
    static int16_t mono[FRAMES_PER_PKT];
    static int32_t stereo[FRAMES_PER_PKT * AUDIO_IO_CHANNELS];

    while (s_session.running) {
        // Block for up to 50 ms waiting for decoded PCM; on timeout, write
        // silence to keep the clock alive.
        size_t want = sizeof(mono);
        size_t got  = xStreamBufferReceive(s_session.playback_buf,
                                           mono, want, pdMS_TO_TICKS(50));
        size_t frames = got / sizeof(int16_t);
        if (frames == 0) {
            memset(mono, 0, sizeof(mono));
            frames = FRAMES_PER_PKT;
        } else if (frames < FRAMES_PER_PKT) {
            // Pad the rest of the packet with silence rather than splitting it.
            memset(mono + frames, 0, sizeof(mono) - got);
            frames = FRAMES_PER_PKT;
        }

        // mono int16 → stereo int32 MSB-aligned. TTS goes on the AEC-ref
        // channel; the other channel is silence (XVF3800 ignores it).
        for (size_t i = 0; i < frames; ++i) {
            int32_t sample = ((int32_t)mono[i]) << 16;
            stereo[i * AUDIO_IO_CHANNELS + AEC_REF_TX_CHANNEL]       = sample;
            stereo[i * AUDIO_IO_CHANNELS + (1 - AEC_REF_TX_CHANNEL)] = 0;
        }
        audio_io_write(stereo, frames);

        // LED state follows recent receive activity.
        TickType_t now = xTaskGetTickCount();
        bool speaking = (now - s_session.last_rx_frame_tick) <
                        pdMS_TO_TICKS(SPEAKING_HOLD_MS);
        if (s_session.connected) {
            leds_set(button_is_muted() ? LED_STATE_MUTED
                     : speaking         ? LED_STATE_SPEAKING
                                        : LED_STATE_LISTENING);
        }
    }
    vTaskDelete(NULL);
}

// ---------- Public API ----------------------------------------------------

esp_err_t webrtc_session_start(const char *backend_url)
{
    if (s_session.running) return ESP_ERR_INVALID_STATE;
    if (!backend_url || !*backend_url) return ESP_ERR_INVALID_ARG;

    s_session.sig = pipecat_signaling_create(backend_url);
    if (!s_session.sig) return ESP_ERR_NO_MEM;

    // Pull TURN/STUN config from the backend's /ice-servers endpoint. With
    // pipecat-in-K8s + STUNner, the backend's SDP advertises a host
    // candidate on the pod IP (unreachable from the ESP32's LAN). The only
    // way through is the relay-relay candidate pair via STUNner — esp_peer
    // gathers its own TURN relay candidate when server_lists is populated.
    // Failure here is non-fatal: degrade to host-only ICE (works on a flat
    // LAN with no NAT between client and server).
    if (pipecat_signaling_fetch_ice_servers(s_session.sig,
                                            &s_session.ice_servers,
                                            &s_session.ice_server_count) != ESP_OK) {
        ESP_LOGW(TAG, "ice-servers fetch failed; continuing host-only");
    }
    if (s_session.ice_server_count) {
        s_session.ice_server_cfgs = calloc(s_session.ice_server_count,
                                            sizeof(esp_peer_ice_server_cfg_t));
        if (s_session.ice_server_cfgs) {
            for (size_t i = 0; i < s_session.ice_server_count; ++i) {
                s_session.ice_server_cfgs[i].stun_url = s_session.ice_servers[i].url;
                s_session.ice_server_cfgs[i].user     = s_session.ice_servers[i].username;
                s_session.ice_server_cfgs[i].psw      = s_session.ice_servers[i].credential;
            }
            ESP_LOGI(TAG, "ICE: %u server(s) configured",
                     (unsigned)s_session.ice_server_count);
        } else {
            s_session.ice_server_count = 0;
        }
    }

    int opus_err = 0;
    s_session.opus_enc = opus_encoder_create(
        AUDIO_IO_SAMPLE_RATE, 1, OPUS_APPLICATION_VOIP, &opus_err);
    if (!s_session.opus_enc || opus_err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_encoder_create failed: %d", opus_err);
        pipecat_signaling_destroy(s_session.sig);
        s_session.sig = NULL;
        return ESP_ERR_NO_MEM;
    }
    opus_encoder_ctl(s_session.opus_enc, OPUS_SET_BITRATE(OPUS_BITRATE_BPS));
    opus_encoder_ctl(s_session.opus_enc, OPUS_SET_COMPLEXITY(OPUS_COMPLEXITY));
    opus_encoder_ctl(s_session.opus_enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    opus_encoder_ctl(s_session.opus_enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(s_session.opus_enc, OPUS_SET_PACKET_LOSS_PERC(10));

    s_session.opus_dec = opus_decoder_create(AUDIO_IO_SAMPLE_RATE, 1, &opus_err);
    if (!s_session.opus_dec || opus_err != OPUS_OK) {
        ESP_LOGE(TAG, "opus_decoder_create failed: %d", opus_err);
        opus_encoder_destroy(s_session.opus_enc);
        s_session.opus_enc = NULL;
        pipecat_signaling_destroy(s_session.sig);
        s_session.sig = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Trigger level = one full playback packet (640 bytes) — otherwise
    // xStreamBufferReceive returns on the first 2 bytes and the playback
    // task pads the rest with silence, producing audible clicking.
    s_session.playback_buf = xStreamBufferCreate(PLAYBACK_BUFFER_BYTES,
                                                  PCM_BYTES_PER_PKT);
    if (!s_session.playback_buf) {
        ESP_LOGE(TAG, "playback buffer alloc failed");
        opus_decoder_destroy(s_session.opus_dec); s_session.opus_dec = NULL;
        opus_encoder_destroy(s_session.opus_enc); s_session.opus_enc = NULL;
        pipecat_signaling_destroy(s_session.sig); s_session.sig = NULL;
        return ESP_ERR_NO_MEM;
    }

    // Self-signed cert generation is expensive; warm it up before opening
    // the first peer connection so the first call doesn't stall the event
    // loop for several seconds.
    esp_peer_pre_generate_cert();

    // Without extra_cfg the default peer implementation falls back to
    // its baked-in defaults (per esp_peer_default.h: 100 KB jitter buffer,
    // 400 KB send pool, 256-slot send queue, 16 max ICE candidates) which
    // blow the heap once Wi-Fi + DTLS are also loaded. Scale down for our
    // audio-only short-burst use case.
    static esp_peer_default_cfg_t peer_default_cfg = {
        .agent_recv_timeout = 100,
        .data_ch_cfg = {
            .recv_cache_size = 4096,    // we don't use data channels
            .send_cache_size = 4096,
        },
        .rtp_cfg = {
            .audio_recv_jitter = { .cache_size = 16 * 1024 },  // ~500 ms @ 24 kbps Opus
            .send_pool_size    = 16 * 1024,
            .send_queue_num    = 32,
        },
        .max_candidates    = 8,
    };

    esp_peer_cfg_t cfg = {
        .server_lists     = s_session.ice_server_cfgs,
        .server_num       = (uint8_t)s_session.ice_server_count,
        .role             = ESP_PEER_ROLE_CONTROLLING,
        .audio_dir        = ESP_PEER_MEDIA_DIR_SEND_RECV,
        .video_dir        = ESP_PEER_MEDIA_DIR_NONE,
        .audio_info       = {
            .codec        = ESP_PEER_AUDIO_CODEC_OPUS,
            .sample_rate  = 16000,
            .channel      = 1,
        },
        .video_info       = { .codec = ESP_PEER_VIDEO_CODEC_NONE },
        // Stay on ALL — RELAY-only causes esp_peer to abort with no binding
        // attempts (state 4 → 8 in 20 ms) when the remote SDP has only host
        // candidates: it apparently won't pair a local relay candidate with
        // a remote host candidate. The proper fix is for the backend's
        // aiortc to also gather a relay candidate via STUNner; then both
        // sides have relay candidates and ICE picks the relay×relay pair.
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .on_state         = on_state,
        .on_msg           = on_msg,
        .on_audio_data    = on_audio_data,
        .extra_cfg        = &peer_default_cfg,
        .extra_size       = sizeof(peer_default_cfg),
    };

    int r = esp_peer_open(&cfg, esp_peer_get_default_impl(), &s_session.peer);
    if (r != 0) {
        ESP_LOGE(TAG, "esp_peer_open failed: %d", r);
        pipecat_signaling_destroy(s_session.sig);
        s_session.sig = NULL;
        return ESP_FAIL;
    }

    s_session.running = true;

    xTaskCreatePinnedToCore(main_loop_task, "rtc_loop",
                            MAIN_LOOP_TASK_STACK, NULL, 7,
                            &s_session.main_loop_task, 0);
    xTaskCreatePinnedToCore(capture_task,   "rtc_cap",
                            CAPTURE_TASK_STACK, NULL, 8,
                            &s_session.capture_task, 1);
    xTaskCreatePinnedToCore(playback_task,  "rtc_play",
                            PLAYBACK_TASK_STACK, NULL, 8,
                            &s_session.playback_task, 1);

    // Triggers offer generation; the resulting SDP arrives via on_msg.
    esp_peer_new_connection(s_session.peer);

    ESP_LOGI(TAG, "session started against %s", backend_url);
    return ESP_OK;
}

void webrtc_session_stop(void)
{
    if (!s_session.running) return;
    s_session.running   = false;
    s_session.connected = false;

    if (s_session.peer) {
        esp_peer_disconnect(s_session.peer);
        esp_peer_close(s_session.peer);
        s_session.peer = NULL;
    }
    if (s_session.playback_buf) {
        vStreamBufferDelete(s_session.playback_buf);
        s_session.playback_buf = NULL;
    }
    if (s_session.opus_dec) {
        opus_decoder_destroy(s_session.opus_dec);
        s_session.opus_dec = NULL;
    }
    if (s_session.opus_enc) {
        opus_encoder_destroy(s_session.opus_enc);
        s_session.opus_enc = NULL;
    }
    if (s_session.sig) {
        pipecat_signaling_destroy(s_session.sig);
        s_session.sig = NULL;
    }
    if (s_session.ice_server_cfgs) {
        free(s_session.ice_server_cfgs);
        s_session.ice_server_cfgs = NULL;
    }
    if (s_session.ice_servers) {
        pipecat_signaling_free_ice_servers(s_session.ice_servers,
                                           s_session.ice_server_count);
        s_session.ice_servers      = NULL;
        s_session.ice_server_count = 0;
    }
    ESP_LOGI(TAG, "session stopped");
}
