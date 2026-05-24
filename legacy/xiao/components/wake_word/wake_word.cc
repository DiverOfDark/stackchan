#include "wake_word.h"

#include <algorithm>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "audio_preprocessor_int8_model_data.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Embedded INT8 streaming MixedNet (baked from main/models/wake_word_ru.tflite
// via tools/embed_tflite.py).
extern const unsigned char wake_word_model_data[];
extern const size_t        wake_word_model_data_len;

static const char *TAG = "wake_word";

// ---- Frontend / preprocessor settings -----------------------------------
//
// micro_speech-style 40-bin mel-style features. 30 ms window, 10 ms stride
// — same as microWakeWord's training-time feature extraction so what the
// device sees at inference time matches what the model was trained on.
#define FRONTEND_SAMPLE_RATE       16000
#define FRONTEND_WINDOW_MS         30
#define FRONTEND_STRIDE_MS         10
#define FRONTEND_WINDOW_SAMPLES    (FRONTEND_SAMPLE_RATE * FRONTEND_WINDOW_MS / 1000)  // 480
#define FRONTEND_STRIDE_SAMPLES    (FRONTEND_SAMPLE_RATE * FRONTEND_STRIDE_MS / 1000)  // 160
#define FRONTEND_FEATURE_SIZE      40    // bins per frame
#define FRONTEND_OVERLAP_SAMPLES   (FRONTEND_WINDOW_SAMPLES - FRONTEND_STRIDE_SAMPLES) // 320

// ---- Detection-side tuning ---------------------------------------------
//
// microWakeWord's reference: average ~10 successive probabilities, fire
// when sustained > threshold. 10 inferences × 10 ms stride = 100 ms of
// sustained confidence before we declare wake.
#define WAKE_WINDOW_LEN     10
#define WAKE_THRESHOLD      0.95f
#define WAKE_COOLDOWN_MS    2000

// Tensor arenas — live in PSRAM. Preprocessor arena sized after the
// upstream micro_speech example (16 KB). Wake-word arena is generous;
// MixedNet at our config uses ~30 KB at peak.
#define PREPROC_ARENA_BYTES   (16 * 1024)
#define WW_ARENA_BYTES        (64 * 1024)

// 18 ops for the audio preprocessor model (must match micro_speech reference).
using PreprocOpResolver = tflite::MicroMutableOpResolver<18>;
// 14 ops for MixedNet streaming inference.
using WakeWordOpResolver = tflite::MicroMutableOpResolver<14>;

static const tflite::Model        *s_preproc_model    = nullptr;
static tflite::MicroInterpreter   *s_preproc          = nullptr;
static uint8_t                    *s_preproc_arena    = nullptr;

static const tflite::Model        *s_ww_model         = nullptr;
static tflite::MicroInterpreter   *s_ww               = nullptr;
static uint8_t                    *s_ww_arena         = nullptr;
static TfLiteTensor               *s_ww_input         = nullptr;
static TfLiteTensor               *s_ww_output        = nullptr;

// Rolling PCM window: at any time holds up to FRONTEND_WINDOW_SAMPLES of
// recent audio. When it fills, we run one preprocessor pass and shift by
// FRONTEND_STRIDE_SAMPLES (10 ms) to set up the next inference.
static int16_t                     s_pcm_window[FRONTEND_WINDOW_SAMPLES];
static size_t                      s_pcm_window_fill = 0;

static float                       s_prob_window[WAKE_WINDOW_LEN] = {0};
static int                         s_prob_window_idx = 0;
static int                         s_prob_window_count = 0;
static atomic_bool                 s_detected_latch = ATOMIC_VAR_INIT(false);
static atomic_int                  s_last_prob_x1000 = ATOMIC_VAR_INIT(0);
static int64_t                     s_last_fire_us = 0;

// ---- Op registration ----------------------------------------------------

static TfLiteStatus register_preproc_ops(PreprocOpResolver &r)
{
    if (r.AddReshape()                       != kTfLiteOk) return kTfLiteError;
    if (r.AddCast()                          != kTfLiteOk) return kTfLiteError;
    if (r.AddStridedSlice()                  != kTfLiteOk) return kTfLiteError;
    if (r.AddConcatenation()                 != kTfLiteOk) return kTfLiteError;
    if (r.AddMul()                           != kTfLiteOk) return kTfLiteError;
    if (r.AddAdd()                           != kTfLiteOk) return kTfLiteError;
    if (r.AddDiv()                           != kTfLiteOk) return kTfLiteError;
    if (r.AddMinimum()                       != kTfLiteOk) return kTfLiteError;
    if (r.AddMaximum()                       != kTfLiteOk) return kTfLiteError;
    if (r.AddWindow()                        != kTfLiteOk) return kTfLiteError;
    if (r.AddFftAutoScale()                  != kTfLiteOk) return kTfLiteError;
    if (r.AddRfft()                          != kTfLiteOk) return kTfLiteError;
    if (r.AddEnergy()                        != kTfLiteOk) return kTfLiteError;
    if (r.AddFilterBank()                    != kTfLiteOk) return kTfLiteError;
    if (r.AddFilterBankSquareRoot()          != kTfLiteOk) return kTfLiteError;
    if (r.AddFilterBankSpectralSubtraction() != kTfLiteOk) return kTfLiteError;
    if (r.AddPCAN()                          != kTfLiteOk) return kTfLiteError;
    if (r.AddFilterBankLog()                 != kTfLiteOk) return kTfLiteError;
    return kTfLiteOk;
}

static TfLiteStatus register_ww_ops(WakeWordOpResolver &r)
{
    if (r.AddReshape()        != kTfLiteOk) return kTfLiteError;
    if (r.AddConv2D()         != kTfLiteOk) return kTfLiteError;
    if (r.AddDepthwiseConv2D()!= kTfLiteOk) return kTfLiteError;
    if (r.AddFullyConnected() != kTfLiteOk) return kTfLiteError;
    if (r.AddAdd()            != kTfLiteOk) return kTfLiteError;
    if (r.AddMul()            != kTfLiteOk) return kTfLiteError;
    if (r.AddMean()           != kTfLiteOk) return kTfLiteError;
    if (r.AddLogistic()       != kTfLiteOk) return kTfLiteError;
    if (r.AddPad()            != kTfLiteOk) return kTfLiteError;
    if (r.AddRelu()           != kTfLiteOk) return kTfLiteError;
    if (r.AddSoftmax()        != kTfLiteOk) return kTfLiteError;
    if (r.AddStridedSlice()   != kTfLiteOk) return kTfLiteError;
    if (r.AddConcatenation()  != kTfLiteOk) return kTfLiteError;
    if (r.AddQuantize()       != kTfLiteOk) return kTfLiteError;
    return kTfLiteOk;
}

// ---- Init ---------------------------------------------------------------

extern "C" esp_err_t wake_word_init(void)
{
    // Preprocessor.
    s_preproc_arena = (uint8_t *)heap_caps_malloc(
        PREPROC_ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_preproc_arena) {
        ESP_LOGE(TAG, "preproc arena alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_preproc_model = tflite::GetModel(g_audio_preprocessor_int8_tflite);
    if (s_preproc_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "preproc schema v%lu, expected v%d",
                 (unsigned long)s_preproc_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }
    static PreprocOpResolver preproc_resolver;
    if (register_preproc_ops(preproc_resolver) != kTfLiteOk) {
        ESP_LOGE(TAG, "preproc op registration failed");
        return ESP_FAIL;
    }
    static tflite::MicroInterpreter preproc_interp(
        s_preproc_model, preproc_resolver, s_preproc_arena, PREPROC_ARENA_BYTES);
    s_preproc = &preproc_interp;
    if (s_preproc->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "preproc AllocateTensors failed");
        return ESP_FAIL;
    }

    // Wake-word model.
    s_ww_arena = (uint8_t *)heap_caps_malloc(
        WW_ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_ww_arena) {
        ESP_LOGE(TAG, "ww arena alloc failed");
        return ESP_ERR_NO_MEM;
    }
    s_ww_model = tflite::GetModel(wake_word_model_data);
    if (s_ww_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "ww schema v%lu, expected v%d",
                 (unsigned long)s_ww_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }
    static WakeWordOpResolver ww_resolver;
    if (register_ww_ops(ww_resolver) != kTfLiteOk) {
        ESP_LOGE(TAG, "ww op registration failed");
        return ESP_FAIL;
    }
    static tflite::MicroInterpreter ww_interp(
        s_ww_model, ww_resolver, s_ww_arena, WW_ARENA_BYTES);
    s_ww = &ww_interp;
    if (s_ww->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "ww AllocateTensors failed");
        return ESP_FAIL;
    }
    s_ww_input  = s_ww->input(0);
    s_ww_output = s_ww->output(0);

    ESP_LOGI(TAG, "preproc ready (arena=%u used)", (unsigned)s_preproc->arena_used_bytes());
    ESP_LOGI(TAG, "wake-word model: %zu bytes, in=%d×%d×%d×%d (%s) → out=%d (%s), arena=%u used",
             wake_word_model_data_len,
             s_ww_input->dims->size > 0 ? s_ww_input->dims->data[0] : 0,
             s_ww_input->dims->size > 1 ? s_ww_input->dims->data[1] : 0,
             s_ww_input->dims->size > 2 ? s_ww_input->dims->data[2] : 0,
             s_ww_input->dims->size > 3 ? s_ww_input->dims->data[3] : 0,
             s_ww_input->type == kTfLiteInt8 ? "int8" : "?",
             s_ww_output->dims->data[s_ww_output->dims->size - 1],
             s_ww_output->type == kTfLiteInt8 ? "int8" : "?",
             (unsigned)s_ww->arena_used_bytes());
    return ESP_OK;
}

// ---- Inference helpers --------------------------------------------------

static TfLiteStatus run_preprocessor(const int16_t *audio_30ms, int8_t *features_out)
{
    TfLiteTensor *in  = s_preproc->input(0);
    TfLiteTensor *out = s_preproc->output(0);
    std::copy_n(audio_30ms, FRONTEND_WINDOW_SAMPLES,
                tflite::GetTensorData<int16_t>(in));
    if (s_preproc->Invoke() != kTfLiteOk) return kTfLiteError;
    std::copy_n(tflite::GetTensorData<int8_t>(out), FRONTEND_FEATURE_SIZE,
                features_out);
    return kTfLiteOk;
}

static void update_window(float prob_float)
{
    s_prob_window[s_prob_window_idx] = prob_float;
    s_prob_window_idx = (s_prob_window_idx + 1) % WAKE_WINDOW_LEN;
    if (s_prob_window_count < WAKE_WINDOW_LEN) s_prob_window_count++;

    atomic_store(&s_last_prob_x1000, (int)(prob_float * 1000.0f));

    if (s_prob_window_count < WAKE_WINDOW_LEN) return;
    float sum = 0;
    for (int i = 0; i < WAKE_WINDOW_LEN; ++i) sum += s_prob_window[i];
    float avg = sum / WAKE_WINDOW_LEN;

    int64_t now_us = esp_timer_get_time();
    if (avg >= WAKE_THRESHOLD &&
        (now_us - s_last_fire_us) > (int64_t)WAKE_COOLDOWN_MS * 1000) {
        s_last_fire_us = now_us;
        atomic_store(&s_detected_latch, true);
        // Damp the window so we don't immediately re-trigger on the same
        // utterance; cooldown is belt-and-suspenders.
        for (int i = 0; i < WAKE_WINDOW_LEN; ++i) s_prob_window[i] = 0;
        s_prob_window_count = 0;
        ESP_LOGI(TAG, "wake!  avg=%.2f", (double)avg);
    }
}

static void run_wake_word(const int8_t *features_40)
{
    // The streaming MixedNet expects one feature slice per invocation; the
    // input tensor size is whatever the trained .tflite declares. Copy as
    // many bytes as fit, zero-pad the rest.
    int8_t *dst = tflite::GetTensorData<int8_t>(s_ww_input);
    const size_t in_bytes = s_ww_input->bytes;
    const size_t to_copy = std::min<size_t>(in_bytes, FRONTEND_FEATURE_SIZE);
    memcpy(dst, features_40, to_copy);
    if (in_bytes > to_copy) memset(dst + to_copy, 0, in_bytes - to_copy);

    if (s_ww->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "ww Invoke failed");
        return;
    }
    // Dequantize the output (int8 → float in [0, 1] after sigmoid).
    int8_t  q     = tflite::GetTensorData<int8_t>(s_ww_output)[0];
    float   scale = s_ww_output->params.scale;
    int     zp    = s_ww_output->params.zero_point;
    float   prob  = scale * (q - zp);
    if (prob < 0) prob = 0;
    if (prob > 1) prob = 1;
    update_window(prob);
}

// ---- Public API ---------------------------------------------------------

extern "C" esp_err_t wake_word_process(const int16_t *pcm, size_t n_samples)
{
    if (!s_preproc || !s_ww) return ESP_ERR_INVALID_STATE;

    while (n_samples > 0) {
        size_t free_in_window = FRONTEND_WINDOW_SAMPLES - s_pcm_window_fill;
        size_t to_copy        = (n_samples < free_in_window) ? n_samples : free_in_window;
        memcpy(s_pcm_window + s_pcm_window_fill, pcm, to_copy * sizeof(int16_t));
        s_pcm_window_fill += to_copy;
        pcm               += to_copy;
        n_samples         -= to_copy;

        if (s_pcm_window_fill == FRONTEND_WINDOW_SAMPLES) {
            int8_t features[FRONTEND_FEATURE_SIZE];
            if (run_preprocessor(s_pcm_window, features) == kTfLiteOk) {
                run_wake_word(features);
            }
            // Slide window forward by stride; keep the overlap tail so the
            // next 10 ms of audio yields the next feature without gaps.
            memmove(s_pcm_window,
                    s_pcm_window + FRONTEND_STRIDE_SAMPLES,
                    FRONTEND_OVERLAP_SAMPLES * sizeof(int16_t));
            s_pcm_window_fill = FRONTEND_OVERLAP_SAMPLES;
        }
    }
    return ESP_OK;
}

extern "C" bool wake_word_detected(void)
{
    return atomic_exchange(&s_detected_latch, false);
}

extern "C" float wake_word_last_probability(void)
{
    return (float)atomic_load(&s_last_prob_x1000) / 1000.0f;
}
