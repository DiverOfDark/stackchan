# Isolated component so esp-tflite-micro's C++ flags don't leak onto the
# pure-C sources in main/. Main just REQUIRES wake_word and gets a clean
# C-callable API back.
#
# Includes TFLite Micro's experimental microfrontend C library (vendored
# at microfrontend/tensorflow/lite/experimental/microfrontend/lib/) for
# audio → spectrogram feature extraction. Using the C frontend rather than
# the audio_preprocessor.tflite model is what microWakeWord training
# expects — same quantization the ESPHome reference impl uses.

set(MFE_LIB microfrontend/tensorflow/lite/experimental/microfrontend/lib)

idf_component_register(
    SRCS
        "wake_word.cc"
        "wake_word_model_data.c"
        # microfrontend C library
        "${MFE_LIB}/frontend.c"
        "${MFE_LIB}/frontend_util.c"
        "${MFE_LIB}/window.c"
        "${MFE_LIB}/window_util.c"
        "${MFE_LIB}/fft.cc"
        "${MFE_LIB}/fft_util.cc"
        "${MFE_LIB}/kiss_fft_int16.cc"
        "${MFE_LIB}/filterbank.c"
        "${MFE_LIB}/filterbank_util.c"
        "${MFE_LIB}/log_lut.c"
        "${MFE_LIB}/log_scale.c"
        "${MFE_LIB}/log_scale_util.c"
        "${MFE_LIB}/noise_reduction.c"
        "${MFE_LIB}/noise_reduction_util.c"
        "${MFE_LIB}/pcan_gain_control.c"
        "${MFE_LIB}/pcan_gain_control_util.c"
    INCLUDE_DIRS
        "."
        "microfrontend"        # makes #include "tensorflow/lite/.../*.h" resolve
                                # AND #include "kiss_fft.h" / "tools/kiss_fftr.h"
    REQUIRES
        espressif__esp-tflite-micro
        esp_timer
)

# The microfrontend + kissfft C sources weren't written with -Werror in mind.
# Suppress the noisy warnings on those files only — keep our own .cc strict.
file(GLOB_RECURSE MFE_SRCS
    "${COMPONENT_DIR}/microfrontend/tensorflow/*.c"
    "${COMPONENT_DIR}/microfrontend/tensorflow/*.cc"
    "${COMPONENT_DIR}/microfrontend/kiss_fft.c"
    "${COMPONENT_DIR}/microfrontend/tools/kiss_fftr.c")
set_source_files_properties(${MFE_SRCS}
    PROPERTIES COMPILE_FLAGS "-Wno-error -Wno-unused-but-set-variable -Wno-unused-function")
