# Thin IDF component shim wrapping aconchillo/libpeer (the open-source
# upstream of Espressif's esp_peer). The library itself lives in
# firmware/deps/libpeer as a git submodule; this shim just registers it
# as an IDF component and pins compile flags.
#
# Modeled on pipecat-esp32/esp32-s3-box-3/components/peer/CMakeLists.txt
# with two deliberate changes:
#   * DISABLE_PEER_SIGNALING=1 (we use our own /api/offer signaling, not
#     libpeer's MQTT/HTTP signaling), which lets us skip the coreHTTP and
#     coreMQTT third_party submodules and their cJSON / mbedTLS overhead.
#   * peer_signaling.c and ssl_transport.c are excluded from the glob so
#     we don't ship empty translation units.

set(LIBPEER_DIR "${CMAKE_CURRENT_LIST_DIR}/../../deps/libpeer")

file(GLOB LIBPEER_SRCS "${LIBPEER_DIR}/src/*.c")
# Drop signaling sources — DISABLE_PEER_SIGNALING=1 already empties them
# at the preprocessor level, but excluding them keeps the build cleaner
# and avoids any future symbol collisions.
list(FILTER LIBPEER_SRCS EXCLUDE REGEX ".*/peer_signaling\\.c$")
list(FILTER LIBPEER_SRCS EXCLUDE REGEX ".*/ssl_transport\\.c$")

idf_component_register(
    SRCS ${LIBPEER_SRCS}
    INCLUDE_DIRS "${LIBPEER_DIR}/src"
    REQUIRES mbedtls srtp lwip esp_netif
)

# Compile-time configuration. Values copied from pipecat-esp32 except
# DISABLE_PEER_SIGNALING which we flip ON (we don't want the built-in
# MQTT/HTTP signaling — the firmware has its own pipecat_signaling.c).
add_compile_definitions(
    ESP32
    CONFIG_USE_LWIP=1
    CONFIG_USE_USRSCTP=0
    CONFIG_AUDIO_BUFFER_SIZE=8096
    CONFIG_DATA_BUFFER_SIZE=102400
    __BYTE_ORDER=__LITTLE_ENDIAN
    DISABLE_PEER_SIGNALING=1
    # Force ECDSA P-256 for the DTLS cert — aiortc (and modern Chrome)
    # reject RSA in WebRTC. Matches pipecat-esp32's setting.
    CONFIG_DTLS_USE_ECDSA=1
    CONFIG_KEEPALIVE_TIMEOUT=0
)
