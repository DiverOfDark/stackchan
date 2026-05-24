# App layer — orchestration, FreeRTOS tasks. Owns the program's lifecycle
# and wires Transport + HAL together using Domain types.

set(APP_SRCS
    # Phase E adds: capture_pipeline.cpp, playback_pipeline.cpp, ui.cpp,
    # provisioning_app.cpp, session.cpp
)

idf_component_register(
    SRCS ${APP_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        pv_domain
        pv_hal
        pv_transport
)
