idf_component_register(
    SRCS "vision.cpp"
    INCLUDE_DIRS "include"
    PRIV_REQUIRES esp_timer
    REQUIRES esp32-camera human_face_detect
)
