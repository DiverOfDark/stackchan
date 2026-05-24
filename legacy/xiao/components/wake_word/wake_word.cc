#include "wake_word.h"

#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "tensorflow/lite/micro/micro_interpreter.h"
#include "tensorflow/lite/micro/micro_log.h"
#include "tensorflow/lite/micro/micro_mutable_op_resolver.h"
#include "tensorflow/lite/schema/schema_generated.h"

// Embedded TFLite model (bake from main/models/wake_word_ru.tflite via
// tools/embed_tflite.py).
extern const unsigned char wake_word_model_data[];
extern const size_t        wake_word_model_data_len;

static const char *TAG = "wake_word";

// Sliding-window detection. microWakeWord's reference is "average of the
// last 10 predictions stays above a probability threshold". 10 inferences
// at one-per-30 ms ≈ 300 ms of sustained confidence before we fire.
#define WAKE_WINDOW_LEN     10
#define WAKE_THRESHOLD      0.95f     // tune per-model (see verification plan)
#define WAKE_COOLDOWN_MS    2000      // suppress re-fire for this long

// Tensor arena — generous because we live in PSRAM and the MixedNet only
// needs a few tens of KB. Resized at init based on actual requirements.
#define TENSOR_ARENA_BYTES  (64 * 1024)

static tflite::MicroInterpreter *s_interpreter      = nullptr;
static const tflite::Model      *s_model            = nullptr;
static uint8_t                  *s_tensor_arena     = nullptr;
static TfLiteTensor             *s_input            = nullptr;
static TfLiteTensor             *s_output           = nullptr;

[[maybe_unused]] static float    s_window[WAKE_WINDOW_LEN] = {0};
[[maybe_unused]] static int      s_window_idx        = 0;
static atomic_bool               s_detected_latch    = ATOMIC_VAR_INIT(false);
static atomic_int                s_last_prob_x1000   = ATOMIC_VAR_INIT(0);
[[maybe_unused]] static uint32_t s_last_fire_ms      = 0;

extern "C" esp_err_t wake_word_init(void)
{
    s_tensor_arena = (uint8_t *)heap_caps_malloc(
        TENSOR_ARENA_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tensor_arena) {
        ESP_LOGE(TAG, "tensor arena alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_model = tflite::GetModel(wake_word_model_data);
    if (s_model->version() != TFLITE_SCHEMA_VERSION) {
        ESP_LOGE(TAG, "model schema v%lu, expected v%d",
                 (unsigned long)s_model->version(), TFLITE_SCHEMA_VERSION);
        return ESP_FAIL;
    }

    // Pull in the small subset of ops MixedNet actually uses. Each op adds
    // ~1-3 KB to the binary; this is the minimum set per microWakeWord's
    // ESPHome reference.
    static tflite::MicroMutableOpResolver<14> resolver;
    resolver.AddReshape();
    resolver.AddConv2D();
    resolver.AddDepthwiseConv2D();
    resolver.AddFullyConnected();
    resolver.AddAdd();
    resolver.AddMul();
    resolver.AddMean();
    resolver.AddLogistic();      // sigmoid head
    resolver.AddPad();
    resolver.AddRelu();
    resolver.AddSoftmax();
    resolver.AddStridedSlice();
    resolver.AddConcatenation();
    resolver.AddQuantize();

    static tflite::MicroInterpreter interp(
        s_model, resolver, s_tensor_arena, TENSOR_ARENA_BYTES);
    s_interpreter = &interp;

    TfLiteStatus alloc = s_interpreter->AllocateTensors();
    if (alloc != kTfLiteOk) {
        ESP_LOGE(TAG, "AllocateTensors failed: %d", (int)alloc);
        return ESP_FAIL;
    }
    s_input  = s_interpreter->input(0);
    s_output = s_interpreter->output(0);

    ESP_LOGI(TAG, "model loaded: %zu bytes, in=%d×%d×%d×%d (%s) → out=%d (%s)",
             wake_word_model_data_len,
             s_input->dims->size > 0 ? s_input->dims->data[0] : 0,
             s_input->dims->size > 1 ? s_input->dims->data[1] : 0,
             s_input->dims->size > 2 ? s_input->dims->data[2] : 0,
             s_input->dims->size > 3 ? s_input->dims->data[3] : 0,
             s_input->type == kTfLiteInt8 ? "int8" : "?",
             s_output->dims->data[s_output->dims->size - 1],
             s_output->type == kTfLiteInt8 ? "int8" : "?");
    return ESP_OK;
}

// ---- Feature extraction (micro_speech-style 40-bin spectrograms) --------
//
// TODO(M6b-followup): wire in the pymicro-features C frontend
// (espressif/esp-microfeatures or ports of TFLite Micro's micro_speech
// example). Until then the model is loaded and verifies allocation/op
// support, but inference is gated off so we don't waste CPU on garbage
// features. Production training already gates on `wake_word_init()` not
// failing, which catches model/op-set mismatches at boot time.

extern "C" esp_err_t wake_word_process(const int16_t *pcm, size_t n_samples)
{
    (void)pcm;
    (void)n_samples;
    if (!s_interpreter) return ESP_ERR_INVALID_STATE;
    // Once features are wired:
    //   1. Slide pcm into a 10 ms frame buffer
    //   2. For each complete frame, run micro_features → 40 bytes into a
    //      circular feature window
    //   3. Every 3 frames, copy the most recent window into s_input, run
    //      s_interpreter->Invoke(), dequantize output, push into s_window
    //   4. If mean(s_window) > WAKE_THRESHOLD and cooldown elapsed,
    //      atomic_store(s_detected_latch, true) and reset s_window
    return ESP_OK;
}

extern "C" bool wake_word_detected(void)
{
    bool yes = atomic_exchange(&s_detected_latch, false);
    return yes;
}

extern "C" float wake_word_last_probability(void)
{
    return (float)atomic_load(&s_last_prob_x1000) / 1000.0f;
}
