#include "webrtc_session.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "opus.h"
#include "peer.h"

#include "audio_io.h"
#include "button.h"
#include "leds.h"
#include "pipecat_signaling.h"
#include "wake_word.h"

static const char *TAG = "webrtc";

// DTLS handshake + ICE state machine recurses deep — 4 KB is not enough,
// hits the stack guard during the SRTP handshake. 10 KB matched esp_peer's
// peer_demo; libpeer is similar shape, keep the same headroom.
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
    PeerConnection        *peer;
    pipecat_signaling_t   *sig;
    // Ownership of the ICE-server backing strings — libpeer's IceServer
    // struct keeps const char* pointers into this list throughout the
    // session, so it must outlive the peer handle. Freed in
    // webrtc_session_stop.
    pipecat_ice_server_t  *ice_servers;
    size_t                 ice_server_count;
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
    // tear down the failed PC and recreate it. libpeer doesn't have an
    // esp_peer-style new_connection-on-existing-handle path — every retry
    // is a fresh peer_connection_create().
    volatile TickType_t    retry_at_tick;
    // The backend's SDP answer arrives on the same task as the
    // onicecandidate callback that issued the POST. We can't call
    // peer_connection_set_remote_description from that callback context
    // (it deadlocks the libpeer loop), so the callback parks the answer
    // here and main_loop_task feeds it in on the next tick.
    char                  *pending_answer;
} session_t;

#define RETRY_INTERVAL_MS   5000

static session_t s_session = {0};

// Global one-time init guard — peer_init / peer_deinit must be balanced
// across the entire lifetime of the firmware, not per session.
static bool s_peer_initialized = false;

// ---------- libpeer callbacks --------------------------------------------

// libpeer ICE/DTLS state transitions. State chart:
//   NEW → CHECKING → CONNECTED → COMPLETED  (happy path)
//   * → FAILED / DISCONNECTED / CLOSED       (error / shutdown)
// CONNECTED means ICE pair selected; COMPLETED means DTLS-SRTP up and
// peer_connection_send_audio will actually transmit. We gate on COMPLETED
// so the first encoded packets aren't silently dropped.
static void on_ice_state(PeerConnectionState state, void *userdata)
{
    ESP_LOGI(TAG, "peer state = %d (%s)", (int)state,
             peer_connection_state_to_string(state));
    switch (state) {
    case PEER_CONNECTION_NEW:
    case PEER_CONNECTION_CHECKING:
    case PEER_CONNECTION_CONNECTED:
        // CONNECTED here means ICE is done but DTLS-SRTP may not yet be up.
        // Keep the "negotiating" LED until COMPLETED to avoid the user
        // thinking they can talk while the first audio packets are still
        // being dropped.
        leds_set(LED_STATE_NEGOTIATING);
        break;
    case PEER_CONNECTION_COMPLETED:
        if (!s_session.connected) {
            ESP_LOGI(TAG, "session ready for media");
        }
        s_session.connected = true;
        leds_set(LED_STATE_LISTENING);
        break;
    case PEER_CONNECTION_FAILED:
    case PEER_CONNECTION_DISCONNECTED:
    case PEER_CONNECTION_CLOSED:
        s_session.connected = false;
        s_session.retry_at_tick =
            xTaskGetTickCount() + pdMS_TO_TICKS(RETRY_INTERVAL_MS);
        leds_set(LED_STATE_CONNECTING);
        break;
    }
}

// libpeer fires this exactly once per peer_connection_create_offer(), after
// all candidates are gathered. The argument is the COMPLETE local SDP with
// every candidate inlined as a=candidate lines — no trickle. We POST it to
// /api/offer and stash the answer for main_loop_task to feed back in.
// Called from libpeer's internal loop; cannot call
// peer_connection_set_remote_description from here (re-enters the lock).
static void on_local_sdp(char *sdp_text, void *userdata)
{
    if (!sdp_text) return;
    ESP_LOGI(TAG, "local SDP gathered (%u bytes)", (unsigned)strlen(sdp_text));

    char *answer_sdp = NULL;
    esp_err_t err = pipecat_signaling_send_offer(
        s_session.sig, sdp_text, &answer_sdp);
    if (err != ESP_OK || !answer_sdp) {
        ESP_LOGE(TAG, "send_offer failed: %s", esp_err_to_name(err));
        // Trigger reconnect — main_loop_task will see retry_at_tick.
        s_session.retry_at_tick =
            xTaskGetTickCount() + pdMS_TO_TICKS(RETRY_INTERVAL_MS);
        return;
    }
    // Hand off to main_loop_task. The previous pending_answer should
    // have been consumed; if not, drop the old one (most recent wins).
    char *old = s_session.pending_answer;
    s_session.pending_answer = answer_sdp;
    free(old);
}

// Inbound TTS audio from the backend. libpeer hands us the bare Opus
// payload (already DePayloaded from RTP). Decode → mono int16 → push to
// the jitter buffer. The dedicated playback task drains it to I2S.
static void on_audio_track(uint8_t *data, size_t size, void *userdata)
{
    if (!data || !s_session.opus_dec) return;

    static int16_t pcm[FRAMES_PER_PKT * 6];   // headroom for any well-formed Opus packet
    int decoded = opus_decode(s_session.opus_dec,
                              data, size,
                              pcm, sizeof(pcm) / sizeof(pcm[0]),
                              /*decode_fec=*/0);
    if (decoded <= 0) {
        ESP_LOGW(TAG, "opus_decode err %d", decoded);
        return;
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
}

// ---------- Peer connection lifecycle -------------------------------------

// Build the PeerConfiguration and create a fresh PeerConnection. Called on
// initial session start AND every retry — libpeer has no "reset existing
// connection" path; every reconnect destroys and recreates.
static int create_peer_connection(void)
{
    PeerConfiguration cfg = {
        .audio_codec  = CODEC_OPUS,
        .video_codec  = CODEC_NONE,
        .datachannel  = DATA_CHANNEL_NONE,   // no RTVI events for now
        .onaudiotrack = on_audio_track,
        .user_data    = NULL,
    };
    // Copy at most 5 ICE servers into the fixed-size array. The strings
    // are owned by s_session.ice_servers and outlive the peer connection.
    size_t n = s_session.ice_server_count;
    if (n > 5) n = 5;
    for (size_t i = 0; i < n; ++i) {
        cfg.ice_servers[i].urls       = s_session.ice_servers[i].url;
        cfg.ice_servers[i].username   = s_session.ice_servers[i].username;
        cfg.ice_servers[i].credential = s_session.ice_servers[i].credential;
    }

    s_session.peer = peer_connection_create(&cfg);
    if (!s_session.peer) {
        ESP_LOGE(TAG, "peer_connection_create failed");
        return -1;
    }
    peer_connection_oniceconnectionstatechange(s_session.peer, on_ice_state);
    peer_connection_onicecandidate(s_session.peer, on_local_sdp);

    // Kicks off ICE gathering + DTLS cert mint. Returns immediately; the
    // gathered SDP is delivered via on_local_sdp once gathering completes.
    peer_connection_create_offer(s_session.peer);
    return 0;
}

static void destroy_peer_connection(void)
{
    if (s_session.peer) {
        peer_connection_close(s_session.peer);
        peer_connection_destroy(s_session.peer);
        s_session.peer = NULL;
    }
    if (s_session.pending_answer) {
        free(s_session.pending_answer);
        s_session.pending_answer = NULL;
    }
}

// ---------- Worker tasks --------------------------------------------------

static void main_loop_task(void *arg)
{
    while (s_session.running) {
        if (s_session.peer) peer_connection_loop(s_session.peer);

        // Feed the SDP answer to libpeer on the loop task (not in the
        // onicecandidate callback context which holds libpeer's lock).
        if (s_session.peer && s_session.pending_answer) {
            char *answer = s_session.pending_answer;
            s_session.pending_answer = NULL;
            peer_connection_set_remote_description(
                s_session.peer, answer, SDP_TYPE_ANSWER);
            free(answer);
        }

        // Recover from a failed/dropped peer connection. libpeer doesn't
        // expose a re-arm on an existing handle — tear down and recreate.
        if (s_session.retry_at_tick &&
            xTaskGetTickCount() >= s_session.retry_at_tick) {
            ESP_LOGI(TAG, "retrying peer connection");
            s_session.retry_at_tick = 0;
            destroy_peer_connection();
            create_peer_connection();
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
        // libpeer takes the bare Opus payload and handles RTP framing +
        // SRTP encryption internally. Return -1 means "not yet connected"
        // or "send queue full"; both transient, not worth logging on miss.
        if (peer_connection_send_audio(s_session.peer, opus_buf, n) < 0) {
            // Silent drop — happens for ~1-2 packets right after COMPLETED
            // while libpeer's send pipeline warms up.
        }
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

    // One-time global init for libpeer (mbedtls + srtp init lives in here).
    if (!s_peer_initialized) {
        if (peer_init() != 0) {
            ESP_LOGE(TAG, "peer_init failed");
            return ESP_FAIL;
        }
        s_peer_initialized = true;
    }

    s_session.sig = pipecat_signaling_create(backend_url);
    if (!s_session.sig) return ESP_ERR_NO_MEM;

    // Pull TURN/STUN config from the backend's /ice-servers endpoint. With
    // pipecat-in-K8s + STUNner, the backend's SDP advertises a host
    // candidate on the pod IP (unreachable from the ESP32's LAN). The only
    // way through is a TURN-relay candidate via STUNner — libpeer gathers
    // its own TURN relay candidate when the IceServer array is populated.
    // Failure here is non-fatal: degrade to host-only ICE (works on a flat
    // LAN with no NAT between client and server).
    if (pipecat_signaling_fetch_ice_servers(s_session.sig,
                                            &s_session.ice_servers,
                                            &s_session.ice_server_count) != ESP_OK) {
        ESP_LOGW(TAG, "ice-servers fetch failed; continuing host-only");
    }
    if (s_session.ice_server_count > 5) {
        ESP_LOGW(TAG, "ICE: %u servers offered, libpeer caps at 5",
                 (unsigned)s_session.ice_server_count);
    }
    if (s_session.ice_server_count) {
        ESP_LOGI(TAG, "ICE: %u server(s) configured",
                 (unsigned)s_session.ice_server_count);
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

    if (create_peer_connection() != 0) {
        vStreamBufferDelete(s_session.playback_buf); s_session.playback_buf = NULL;
        opus_decoder_destroy(s_session.opus_dec); s_session.opus_dec = NULL;
        opus_encoder_destroy(s_session.opus_enc); s_session.opus_enc = NULL;
        pipecat_signaling_destroy(s_session.sig); s_session.sig = NULL;
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

    ESP_LOGI(TAG, "session started against %s", backend_url);
    return ESP_OK;
}

void webrtc_session_stop(void)
{
    if (!s_session.running) return;
    s_session.running   = false;
    s_session.connected = false;

    destroy_peer_connection();
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
    if (s_session.ice_servers) {
        pipecat_signaling_free_ice_servers(s_session.ice_servers,
                                           s_session.ice_server_count);
        s_session.ice_servers      = NULL;
        s_session.ice_server_count = 0;
    }
    ESP_LOGI(TAG, "session stopped");
}
