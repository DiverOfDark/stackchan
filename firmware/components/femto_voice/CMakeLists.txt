idf_component_register(
    SRCS "audio_cores3.cpp" "session.cpp" "ws_link.cpp" "femto_voice.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pv_domain wake_word esp_driver_i2s
    PRIV_REQUIRES espressif__esp_codec_dev espressif__esp_websocket_client esp_timer esp_wifi mbedtls
)

# esp_codec_dev in legacy-I2C mode includes driver/i2c.h (IDF "driver" compat
# component) without declaring the dependency.
idf_component_get_property(codec_lib espressif__esp_codec_dev COMPONENT_LIB)
target_link_libraries(${codec_lib} PRIVATE idf::driver)
