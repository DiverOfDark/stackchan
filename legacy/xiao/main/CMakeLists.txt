# Thin app_main + the embedded captive-portal HTML blob. All real
# code lives in components/pv_{domain,hal,transport,app}/.
idf_component_register(
    SRCS
        "main.cpp"
        "captive_portal_index_html.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_driver_i2c
        nvs_flash
        pv_hal
        pv_transport
        pv_app
)
