# App layer — orchestration, FreeRTOS tasks. Owns the program's lifecycle
# and wires Transport + HAL together using Domain types.

set(APP_SRCS
    "src/ui.cpp"
    "src/session.cpp"
    "src/web_server.cpp"
)

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        pv_domain
        pv_hal
        pv_transport
        esp_http_server
        app_update
)
