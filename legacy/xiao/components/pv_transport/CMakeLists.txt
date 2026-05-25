# Transport layer — RAII C++17 wrappers around libpeer / micro-opus /
# esp-tflite-micro / esp_http_client. Each class hides the C void* +
# C-function-pointer-callback dance behind typed std::function events.

set(TRANSPORT_SRCS
    "src/opus_codec.cpp"
    "src/signaling.cpp"
    "src/wake_engine.cpp"
    "src/peer.cpp"
)

idf_component_register(
    SRCS ${TRANSPORT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        pv_domain
        pv_hal
        libpeer
        esphome__micro-opus
        wake_word
    PRIV_REQUIRES
        espressif__cjson
)
