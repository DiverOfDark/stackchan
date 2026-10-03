idf_component_register(
    SRCS "audio_cores3.cpp" "session.cpp" "femto_voice.cpp"
    INCLUDE_DIRS "include"
    REQUIRES pv_domain pv_transport libpeer wake_word esp_driver_i2s
    PRIV_REQUIRES espressif__esp_codec_dev esp_timer
)

# libsrtp / libpeer predate GCC 14, which made incompatible pointer types an
# error (uint32_t* vs unsigned int* on Xtensa). Same code, just not -Werror.
foreach(dep sepfy__srtp libpeer)
    idf_component_get_property(lib ${dep} COMPONENT_LIB)
    target_compile_options(${lib} PRIVATE -Wno-error=incompatible-pointer-types -Wno-incompatible-pointer-types)
endforeach()

# esp_codec_dev in legacy-I2C mode includes driver/i2c.h (IDF "driver" compat
# component) without declaring the dependency.
idf_component_get_property(codec_lib espressif__esp_codec_dev COMPONENT_LIB)
target_link_libraries(${codec_lib} PRIVATE idf::driver)
