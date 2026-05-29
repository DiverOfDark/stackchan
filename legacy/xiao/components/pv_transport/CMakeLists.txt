# Transport layer — RAII C++17 wrappers around libpeer / esp-tflite-micro /
# esp_http_client. Each class hides the C void* + C-function-pointer-callback
# dance behind typed std::function events. (Audio on the wire is G.711 µ-law,
# companded in pv_domain/g711.hpp — no Opus dependency.)

set(TRANSPORT_SRCS
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
        wake_word
    PRIV_REQUIRES
        espressif__cjson
)
