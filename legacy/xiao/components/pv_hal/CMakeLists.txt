# HAL layer — RAII C++17 wrappers around ESP-IDF C APIs.
#
# Each class here owns one ESP-IDF resource (i2c device, i2s channel, GPIO,
# nvs handle, wifi netif, ...) and releases it in the destructor. No domain
# logic lives here — that's the domain/ layer.

set(HAL_SRCS
    "src/xvf3800.cpp"
    # Phase C adds: i2s_mic.cpp, i2s_speaker.cpp, button_gpio.cpp,
    # nvs_kv.cpp, sntp.cpp, https_client.cpp, wifi_sta.cpp, softap_portal.cpp
)

idf_component_register(
    SRCS ${HAL_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        pv_domain
        esp_driver_i2c
        # Phase C will add: esp_driver_i2s, esp_driver_gpio, esp_event,
        # esp_http_client, esp_http_server, esp-tls, esp_netif, esp_wifi,
        # lwip, nvs_flash
)
