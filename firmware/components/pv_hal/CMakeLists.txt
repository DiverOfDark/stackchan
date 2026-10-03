# Trimmed from pipecat-voice-assistant's pv_hal: only the HTTPS client
# (used by pv_transport's signaling). The rest is replaced by the Rust
# firmware and femto_voice's CoreS3 audio.
idf_component_register(
    SRCS "src/https_client.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_client esp-tls
)
