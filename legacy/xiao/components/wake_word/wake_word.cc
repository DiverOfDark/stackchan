#include "wake_word.h"

#include <algorithm>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "tensorflow/lite/micro/micro_allocator.h"
#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/micro/micro_resource_variable.h"
#include "tensorflow/lite/schema/schema_generated.h"

// TFLite Micro's experimental microfrontend C library (vendored under
// microfrontend/tensorflow/lite/...) for log-mel spectrogram features.
// Matches what pymicro-features produces during microWakeWord training,
// which is what the model was actually trained on.
extern "C" {
#include "tensorflow/lite/experimental/microfrontend/lib/frontend.h"
#include "tensorflow/lite/experimental/microfrontend/lib/frontend_util.h"
}

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

// ---- Detection-side tuning ---------------------------------------------
//
// microWakeWord's reference averages ~10 successive probabilities and fires
// when the MEAN stays above threshold — which assumes a well-trained model
// that holds high confidence across the whole wake word. Ours doesn't: it
// was trained from ~15 real samples + augmentation and only spikes to
// p_max ≈ 0.12 for a couple of invokes during the word, sitting near 0
// otherwise. A mean-over-window detector dilutes that burst to ~0.03 and
// never fires (this was the main cause of the ~1-in-5 hit rate).
//
// Instead we count how many recent invokes cleared the threshold and fire
// when at least WAKE_MIN_HITS of them did within a WAKE_WINDOW_LEN window
// (~150 ms of invokes). That catches the spike while a lone-frame blip from
// random loud speech still gets rejected. Retrain with more data and this
// can move back to a higher threshold + mean detector.
#define WAKE_WINDOW_LEN     5
#define WAKE_MIN_HITS       2
// Two gates, tuned from real device captures:
//   real wakes: avg 0.61 win=[0.24 0.50 0.68 0.76 0.87]  (smooth rise)
//               avg 0.57 win=[0.91 0.03 0.18 0.82 0.93]  (spiky, two clusters)
//   false pos : avg 0.45 win=[0.82 0.76 0.02 0.13 0.50]  (spike then collapse)
// Both real and false clear the per-frame hit-count, so threshold + hit-count
// alone can't tell them apart — but the window MEAN does: a real wake keeps
// confidence up across the window (mean ~0.57–0.61), a false blip spikes then
// collapses (mean ~0.45). So:
//   WAKE_THRESHOLD — per-frame bar to count as a hit. 0.60 so real wakes
//                    register a touch earlier (better recall).
//   WAKE_MIN_HITS  — how many frames must clear it (catches the spike cluster).
//   WAKE_AVG_MIN   — the window mean must also clear this. This rejects the
//                    collapse-transient false positive while passing real
//                    wakes. Set to 0.50 — midway in the observed real/false gap
//                    (reals 0.57/0.61, false 0.45), giving reals ~0.07–0.11
//                    margin and still cutting the false (0.45). Watch the avg=…
//                    field in the "wake!" log on real vs false fires to refine.
#define WAKE_THRESHOLD      0.60f
#define WAKE_AVG_MIN        0.50f
#define WAKE_COOLDOWN_MS    2000

// Periodic diagnostic logs — confirm mic is alive and model is producing
// reasonable probabilities while saying / not saying the wake phrase.
// Off by default in shipped firmware (three streams of LOGI/sec at idle
// drowns out everything else). Flip to 1 locally when debugging.
#define WAKE_DIAG_ENABLE         0
#define WAKE_DIAG_PERIOD_FRAMES  100   // ~1 s @ 10 ms feature stride

// Wake-word arena lives in PSRAM. MixedNet at our config uses ~30 KB peak.
#define WW_ARENA_BYTES        (64 * 1024)

// Ops for MixedNet streaming inference. Confirmed against our trained
// model with `tf.lite.Interpreter._get_ops_details()`: Conv2D /
// DepthwiseConv2D / FullyConnected / Reshape / Logistic / Concatenation /
// StridedSlice / SplitV / Quantize + the four stateful-variable ops
// (CallOnce / VarHandle / AssignVariable / ReadVariable). The extras
// (Add/Mul/Mean/Pad/Relu/Softmax) are unused by the current model but
// stay registered as cheap insurance against a retraining run picking
// them up.
using WakeWordOpResolver = tflite::MicroMutableOpResolver<19>;

// Microfrontend (log-mel spectrogram) state. Configured at init from
// FRONTEND_* constants in wake_word.h.
static FrontendConfig              s_frontend_cfg;
static FrontendState               s_frontend_state;

static const tflite::Model        *s_ww_model         = nullptr;
static tflite::MicroInterpreter   *s_ww               = nullptr;
static uint8_t                    *s_ww_arena         = nullptr;
static TfLiteTensor               *s_ww_input         = nullptr;
static TfLiteTensor               *s_ww_output        = nullptr;
static size_t                      s_ww_input_slices  = 1;   // = input.bytes / 40, set at init
// Combined feature → int8 transform, derived at init from the model's own
// input quantization params (matches microWakeWord's quantize_input_data).
// Formula: int8 = round(uint16_frontend_value * k_mult + k_offset).
static float                       s_quant_mult       = 0.0f;
static float                       s_quant_offset     = 0.0f;
// Slice-filling buffer between FrontendProcessSamples emits and an Invoke.
// Streaming MixedNet wants s_ww_input_slices NEW feature slices per Invoke;
// we accumulate them here. WW_MAX_INPUT_SLICES is the upper bound supported
// by this buffer — wake_word_init() refuses to load a model whose tensor
// shape would exceed it, so the writes in run_wake_word() / wake_word_process
// can't overflow.
#define WW_MAX_INPUT_SLICES         8
static int8_t                      s_pending_features[40 * WW_MAX_INPUT_SLICES];
static size_t                      s_pending_count    = 0;

// (No PCM window state — FrontendProcessSamples buffers internally.)

static float                       s_prob_window[WAKE_WINDOW_LEN] = {0};
static int                         s_prob_window_idx = 0;
static int                         s_prob_window_count = 0;
static atomic_bool                 s_detected_latch = ATOMIC_VAR_INIT(false);
static atomic_int                  s_last_prob_x1000 = ATOMIC_VAR_INIT(0);

// Diagnostic accumulators (reset every WAKE_DIAG_PERIOD_FRAMES).
static float                       s_diag_max_prob = 0.0f;
static uint64_t                    s_diag_rms_sumsq = 0;
static uint32_t                    s_diag_rms_count = 0;
static int                         s_diag_frame_counter = 0;
static int64_t                     s_last_fire_us = 0;

// ---- Op registration ----------------------------------------------------

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
    if (r.AddSplitV()         != kTfLiteOk) return kTfLiteError;
    // Streaming-state ops — microWakeWord uses TF resource variables for
    // the per-layer ring buffers that carry temporal context across Invokes.
    if (r.AddCallOnce()       != kTfLiteOk) return kTfLiteError;
    if (r.AddVarHandle()      != kTfLiteOk) return kTfLiteError;
    if (r.AddAssignVariable() != kTfLiteOk) return kTfLiteError;
    if (r.AddReadVariable()   != kTfLiteOk) return kTfLiteError;
    return kTfLiteOk;
}

// ---- Init ---------------------------------------------------------------

extern "C" esp_err_t wake_word_init(void)
{
    // Configure the microfrontend the same way microWakeWord training does
    // (matches pymicro-features / ESPHome reference). Each call to
    // FrontendProcessSamples consumes up to FRONTEND_WINDOW_MS of audio
    // and emits one 40-bin uint16 spectrogram slice every FRONTEND_STRIDE_MS.
    FrontendFillConfigWithDefaults(&s_frontend_cfg);
    s_frontend_cfg.window.size_ms                   = FRONTEND_WINDOW_MS;     // 30 ms
    s_frontend_cfg.window.step_size_ms              = FRONTEND_STRIDE_MS;     // 10 ms
    s_frontend_cfg.filterbank.num_channels          = FRONTEND_FEATURE_SIZE;  // 40
    s_frontend_cfg.filterbank.lower_band_limit      = 125.0f;
    s_frontend_cfg.filterbank.upper_band_limit      = 7500.0f;
    s_frontend_cfg.noise_reduction.smoothing_bits   = 10;
    s_frontend_cfg.noise_reduction.even_smoothing   = 0.025f;
    s_frontend_cfg.noise_reduction.odd_smoothing    = 0.06f;
    s_frontend_cfg.noise_reduction.min_signal_remaining = 0.05f;
    s_frontend_cfg.pcan_gain_control.enable_pcan        = 1;
    s_frontend_cfg.pcan_gain_control.strength           = 0.95f;
    s_frontend_cfg.pcan_gain_control.offset             = 80.0f;
    s_frontend_cfg.pcan_gain_control.gain_bits          = 21;
    s_frontend_cfg.log_scale.enable_log                 = 1;
    s_frontend_cfg.log_scale.scale_shift                = 6;
    if (!FrontendPopulateState(&s_frontend_cfg, &s_frontend_state,
                               FRONTEND_SAMPLE_RATE)) {
        ESP_LOGE(TAG, "FrontendPopulateState failed");
        return ESP_FAIL;
    }

    // Wake-word model.
    s_ww_arena = (uint8_t *)heap_caps_aligned_alloc(
        16, WW_ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    // microWakeWord's streaming graph uses TF resource variables for its
    // per-layer ring buffers, so the interpreter needs a MicroResourceVariables
    // pool — without it VAR_HANDLE op prepares fail at AllocateTensors. We
    // build one via the explicit-allocator constructor.
    constexpr int kNumResourceVariables = 24;   // microWakeWord MixedNet has ~16; pad
    tflite::MicroAllocator *allocator =
        tflite::MicroAllocator::Create(s_ww_arena, WW_ARENA_BYTES);
    if (!allocator) {
        ESP_LOGE(TAG, "ww MicroAllocator::Create failed");
        return ESP_FAIL;
    }
    tflite::MicroResourceVariables *resource_vars =
        tflite::MicroResourceVariables::Create(allocator, kNumResourceVariables);
    if (!resource_vars) {
        ESP_LOGE(TAG, "ww MicroResourceVariables::Create failed");
        return ESP_FAIL;
    }
    static tflite::MicroInterpreter ww_interp(
        s_ww_model, ww_resolver, allocator, resource_vars);
    s_ww = &ww_interp;
    if (s_ww->AllocateTensors() != kTfLiteOk) {
        ESP_LOGE(TAG, "ww AllocateTensors failed");
        return ESP_FAIL;
    }
    s_ww_input  = s_ww->input(0);
    s_ww_output = s_ww->output(0);
    // microWakeWord's streaming MixedNet declares an input tensor sized for
    // N consecutive 40-bin feature slices (typically 3 — i.e. a 30 ms
    // context window per Invoke). Derive N at runtime so a retrain with a
    // different stride / receptive field Just Works.
    if (s_ww_input->bytes % FRONTEND_FEATURE_SIZE != 0) {
        ESP_LOGE(TAG, "ww input %u bytes not multiple of feature size %d",
                 (unsigned)s_ww_input->bytes, FRONTEND_FEATURE_SIZE);
        return ESP_FAIL;
    }
    s_ww_input_slices = s_ww_input->bytes / FRONTEND_FEATURE_SIZE;
    if (s_ww_input_slices > WW_MAX_INPUT_SLICES) {
        ESP_LOGE(TAG, "ww model needs %u input slices, s_pending_features holds %d — "
                      "bump WW_MAX_INPUT_SLICES",
                 (unsigned)s_ww_input_slices, WW_MAX_INPUT_SLICES);
        return ESP_FAIL;
    }

    // The C microfrontend lib emits uint16 spectrogram values in roughly
    // 0..670 range. pymicro-features (which microWakeWord training uses)
    // returns the same values pre-scaled by 0.0390625 (= 1/25.6), giving
    // floats in 0..26. The model's int8 input quantization then maps that
    // float range into [-128, 127] via int8 = round(f / scale + zp).
    // Combine: int8 = round(uint16 * (0.0390625 / scale) + zp).
    s_quant_mult   = 0.0390625f / s_ww_input->params.scale;
    s_quant_offset = (float)s_ww_input->params.zero_point;
    ESP_LOGI(TAG, "input quant: scale=%.5f zp=%d → feature mult=%.4f offset=%.1f",
             (double)s_ww_input->params.scale, s_ww_input->params.zero_point,
             (double)s_quant_mult, (double)s_quant_offset);

    ESP_LOGI(TAG, "microfrontend ready (40-bin, %d ms window, %d ms stride)",
             FRONTEND_WINDOW_MS, FRONTEND_STRIDE_MS);
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

static void update_window(float prob_float)
{
    s_prob_window[s_prob_window_idx] = prob_float;
    s_prob_window_idx = (s_prob_window_idx + 1) % WAKE_WINDOW_LEN;
    if (s_prob_window_count < WAKE_WINDOW_LEN) s_prob_window_count++;

    atomic_store(&s_last_prob_x1000, (int)(prob_float * 1000.0f));
#if WAKE_DIAG_ENABLE
    if (prob_float > s_diag_max_prob) s_diag_max_prob = prob_float;
    if (++s_diag_frame_counter >= WAKE_DIAG_PERIOD_FRAMES) {
        float avg = 0;
        for (int i = 0; i < s_prob_window_count; ++i) avg += s_prob_window[i];
        if (s_prob_window_count) avg /= s_prob_window_count;
        float rms = 0;
        if (s_diag_rms_count) {
            rms = sqrtf((float)s_diag_rms_sumsq / (float)s_diag_rms_count);
        }
        ESP_LOGI(TAG, "diag: p_now=%.2f p_max=%.2f p_avg10=%.2f rms=%.0f (%lu frames)",
                 (double)prob_float, (double)s_diag_max_prob, (double)avg,
                 (double)rms, (unsigned long)s_diag_rms_count);
        s_diag_max_prob      = 0.0f;
        s_diag_rms_sumsq     = 0;
        s_diag_rms_count     = 0;
        s_diag_frame_counter = 0;
    }
#endif

    if (s_prob_window_count < WAKE_WINDOW_LEN) return;
    // Fire on a burst of spikes, not on a sustained mean — see the tuning
    // note above. Count threshold crossings in the window (and track the
    // peak just for the log line).
    int   hits = 0;
    float peak = 0.0f, sum = 0.0f;
    for (int i = 0; i < WAKE_WINDOW_LEN; ++i) {
        const float p = s_prob_window[i];
        if (p >= WAKE_THRESHOLD) ++hits;
        if (p > peak) peak = p;
        sum += p;
    }
    const float avg = sum / WAKE_WINDOW_LEN;

    int64_t now_us = esp_timer_get_time();
    if (hits >= WAKE_MIN_HITS && avg >= WAKE_AVG_MIN &&
        (now_us - s_last_fire_us) > (int64_t)WAKE_COOLDOWN_MS * 1000) {
        // Snapshot the window probabilities for the log before we damp it.
        float w[WAKE_WINDOW_LEN];
        for (int i = 0; i < WAKE_WINDOW_LEN; ++i) w[i] = s_prob_window[i];
        s_last_fire_us = now_us;
        atomic_store(&s_detected_latch, true);
        // Damp the window so we don't immediately re-trigger on the same
        // utterance; cooldown is belt-and-suspenders.
        for (int i = 0; i < WAKE_WINDOW_LEN; ++i) s_prob_window[i] = 0;
        s_prob_window_count = 0;
        // Always log the firing probabilities (peak + mean + threshold + the
        // whole window) so the operating point can be judged from real fires.
        // The %.2f count below matches WAKE_WINDOW_LEN (5).
        ESP_LOGI(TAG, "wake!  hits=%d/%d peak=%.2f avg=%.2f thr=%.2f win=[%.2f %.2f %.2f %.2f %.2f]",
                 hits, WAKE_WINDOW_LEN, (double)peak, (double)avg, (double)WAKE_THRESHOLD,
                 (double)w[0], (double)w[1], (double)w[2], (double)w[3], (double)w[4]);
    }
}

static void run_wake_word(void)
{
    // The streaming MixedNet consumes s_ww_input_slices NEW feature slices
    // per Invoke (it maintains its own context across calls via
    // VAR_HANDLE-backed resource variables). Shifting the window by 1 slice
    // and re-running on overlapping data — which my original code did —
    // corrupts that state and the model never fires. We accumulate exactly
    // s_ww_input_slices fresh slices in s_pending_features and copy them in
    // as a non-overlapping block.
    int8_t *dst = tflite::GetTensorData<int8_t>(s_ww_input);
    memcpy(dst, s_pending_features, s_ww_input_slices * FRONTEND_FEATURE_SIZE);

#if WAKE_DIAG_ENABLE
    // Diagnostic: log feature stats + raw output once per second.
    static int s_invoke_counter = 0;
    bool emit_diag = (++s_invoke_counter >= 33);   // ~1 s @ 30 ms/inference
    int feat_min = 127, feat_max = -128;
    if (emit_diag) {
        const int8_t *last = s_pending_features + (s_ww_input_slices - 1) * FRONTEND_FEATURE_SIZE;
        for (int i = 0; i < FRONTEND_FEATURE_SIZE; ++i) {
            if (last[i] < feat_min) feat_min = last[i];
            if (last[i] > feat_max) feat_max = last[i];
        }
    }
#endif

    if (s_ww->Invoke() != kTfLiteOk) {
        ESP_LOGW(TAG, "ww Invoke failed");
        return;
    }
    // Output is uint8 (per the trained .tflite). raw is in [0, 255].
    // Dequantize: prob = scale * (raw - zp). For sigmoid head with scale
    // ≈ 1/256 and zp = 0, this gives prob in [0, 1].
    uint8_t raw   = tflite::GetTensorData<uint8_t>(s_ww_output)[0];
    float   scale = s_ww_output->params.scale;
    int     zp    = s_ww_output->params.zero_point;
    float   prob  = scale * ((int)raw - zp);
    if (prob < 0) prob = 0;
    if (prob > 1) prob = 1;

#if WAKE_DIAG_ENABLE
    if (emit_diag) {
        static uint8_t s_raw_max = 0;
        if (raw > s_raw_max) s_raw_max = raw;
        ESP_LOGI(TAG, "diag(model): feat min=%d max=%d → raw=%u raw_max_ever=%u p=%.3f",
                 feat_min, feat_max, (unsigned)raw, (unsigned)s_raw_max, (double)prob);
        s_invoke_counter = 0;
    }
#endif
    update_window(prob);
}

// ---- Public API ---------------------------------------------------------

extern "C" esp_err_t wake_word_process(const int16_t *pcm, size_t n_samples)
{
    if (!s_ww) return ESP_ERR_INVALID_STATE;

#if WAKE_DIAG_ENABLE
    // Diagnostic: RMS + peak of incoming PCM so we can confirm the mic
    // is alive separately from whether the model fires.
    int32_t peak = 0;
    for (size_t i = 0; i < n_samples; ++i) {
        int32_t s = pcm[i];
        s_diag_rms_sumsq += (uint64_t)(s * s);
        int32_t a = s < 0 ? -s : s;
        if (a > peak) peak = a;
    }
    s_diag_rms_count += n_samples;
    static int s_caller_counter = 0;
    if ((s_caller_counter += n_samples) >= 16000) {
        ESP_LOGI(TAG, "diag(input): rms=%.0f peak=%ld samples=%lu p_last=%.2f",
                 s_diag_rms_count ?
                     (double)sqrtf((float)s_diag_rms_sumsq / (float)s_diag_rms_count) : 0.0,
                 (long)peak,
                 (unsigned long)s_diag_rms_count,
                 (double)wake_word_last_probability());
        s_caller_counter   = 0;
        s_diag_rms_sumsq   = 0;
        s_diag_rms_count   = 0;
    }
#endif

    // The frontend consumes audio in arbitrary chunks and emits one feature
    // slice per FRONTEND_STRIDE_MS of audio internally — we just keep
    // feeding it. Each slice is appended to s_pending_features; when we
    // have s_ww_input_slices fresh slices we Invoke the model with them
    // and reset the pending buffer for the next batch.
    size_t remaining = n_samples;
    const int16_t *p = pcm;
    while (remaining > 0) {
        size_t consumed = 0;
        FrontendOutput out = FrontendProcessSamples(
            &s_frontend_state, p, remaining, &consumed);
        if (consumed == 0) break;
        p         += consumed;
        remaining -= consumed;
        if (out.size == 0 || out.values == nullptr) continue;

        // microWakeWord training feeds pymicro-features floats (0..~26)
        // directly through the model's input quantization:
        //   int8 = round(uint16_value * 0.0390625 / input_scale + input_zp)
        // (s_quant_mult / s_quant_offset precomputed at init from the
        // model's params). The earlier ESPHome `(u*256+333)/666` formula
        // assumed input was uint16 0..670 — fine in the abstract, but
        // doesn't actually match what training fed the model.
        int8_t *dst = &s_pending_features[s_pending_count * FRONTEND_FEATURE_SIZE];
        for (size_t i = 0; i < (size_t)FRONTEND_FEATURE_SIZE && i < out.size; ++i) {
            int32_t v = (int32_t)lroundf((float)out.values[i] * s_quant_mult
                                          + s_quant_offset);
            if (v < INT8_MIN) v = INT8_MIN;
            if (v > INT8_MAX) v = INT8_MAX;
            dst[i] = (int8_t)v;
        }
        if (++s_pending_count >= s_ww_input_slices) {
            run_wake_word();
            s_pending_count = 0;
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
