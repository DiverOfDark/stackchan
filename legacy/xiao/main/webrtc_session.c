#include "webrtc_session.h"

#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_peer.h"
#include "esp_peer_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "audio_io.h"
#include "pipecat_signaling.h"

static const char *TAG = "webrtc";

#define MAIN_LOOP_TASK_STACK   4096
#define CAPTURE_TASK_STACK     8192
#define CAPTURE_FRAMES_PER_PKT 320   // 20 ms @ 16 kHz

typedef struct {
    esp_peer_handle_t      peer;
    pipecat_signaling_t   *sig;
    TaskHandle_t           main_loop_task;
    TaskHandle_t           capture_task;
    volatile bool          running;
    volatile bool          connected;
} session_t;

static session_t s_session = {0};

// ---------- esp_peer callbacks --------------------------------------------

static int on_state(esp_peer_state_t state, void *ctx)
{
    ESP_LOGI(TAG, "peer state = %d", (int)state);
    if (state == ESP_PEER_STATE_CONNECTED) {
        s_session.connected = true;
    } else if (state == ESP_PEER_STATE_DISCONNECTED ||
               state == ESP_PEER_STATE_CLOSED ||
               state == ESP_PEER_STATE_CONNECT_FAILED) {
        s_session.connected = false;
    }
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
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    vTaskDelete(NULL);
}

static void capture_task(void *arg)
{
    // Reads stereo 32-bit frames from XVF3800, downmixes to mono int16
    // because the Opus encoder we'll add wants 16-bit mono PCM.
    static int32_t stereo[CAPTURE_FRAMES_PER_PKT * AUDIO_IO_CHANNELS];
    static int16_t mono[CAPTURE_FRAMES_PER_PKT];  // fed to Opus encoder (M4 follow-up)
    (void)mono;

    while (s_session.running) {
        if (!s_session.connected) {
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }
        if (audio_io_read(stereo, CAPTURE_FRAMES_PER_PKT) != ESP_OK) {
            continue;
        }
        // Channel 0 of XVF3800 = processed mono audio. 32-bit MSB-aligned →
        // shift to int16. (XMOS conventionally outputs Q1.31; right-shift 16.)
        for (size_t i = 0; i < CAPTURE_FRAMES_PER_PKT; ++i) {
            mono[i] = (int16_t)(stereo[i * AUDIO_IO_CHANNELS] >> 16);
        }

        // TODO(M4-followup): Opus-encode `mono` and hand to esp_peer_send_audio.
        // Need to add an Opus encoder dependency first — espressif/esp_audio_codec
        // is the standard choice. Once wired:
        //   uint8_t opus_buf[600];
        //   int opus_len = opus_encode(enc, mono, CAPTURE_FRAMES_PER_PKT,
        //                              opus_buf, sizeof(opus_buf));
        //   esp_peer_audio_frame_t f = {.pts = ..., .data = opus_buf, .size = opus_len};
        //   esp_peer_send_audio(s_session.peer, &f);
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

    // Self-signed cert generation is expensive; warm it up before opening
    // the first peer connection so the first call doesn't stall the event
    // loop for several seconds.
    esp_peer_pre_generate_cert();

    esp_peer_cfg_t cfg = {
        .role             = ESP_PEER_ROLE_CONTROLLING,
        .audio_dir        = ESP_PEER_MEDIA_DIR_SEND_RECV,
        .video_dir        = ESP_PEER_MEDIA_DIR_NONE,
        .audio_info       = {
            .codec        = ESP_PEER_AUDIO_CODEC_OPUS,
            .sample_rate  = 16000,
            .channel      = 1,
        },
        .video_info       = { .codec = ESP_PEER_VIDEO_CODEC_NONE },
        .ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL,
        .on_state         = on_state,
        .on_msg           = on_msg,
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
    if (s_session.sig) {
        pipecat_signaling_destroy(s_session.sig);
        s_session.sig = NULL;
    }
    ESP_LOGI(TAG, "session stopped");
}
