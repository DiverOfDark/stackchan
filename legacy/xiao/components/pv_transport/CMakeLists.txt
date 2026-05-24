# Transport layer — RAII C++17 wrappers around libpeer / micro-opus /
# esp-tflite-micro / esp_http_client. Each class hides the C void* +
# C-function-pointer-callback dance behind typed std::function events.

set(TRANSPORT_SRCS
    # Phase D adds: peer.cpp, opus_codec.cpp, wake_engine.cpp, signaling.cpp
)

idf_component_register(
    SRCS ${TRANSPORT_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        pv_domain
        pv_hal
        # Phase D will add: libpeer, esphome__micro-opus,
        # espressif__esp-tflite-micro
)
