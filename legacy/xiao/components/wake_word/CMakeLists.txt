# Isolated component so esp-tflite-micro's C++ flags don't leak onto the
# pure-C sources in main/. Main just REQUIRES wake_word and gets a clean
# C-callable API back.
idf_component_register(
    SRCS
        "wake_word.cc"
        "wake_word_model_data.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        espressif__esp-tflite-micro
        esp_timer
)
