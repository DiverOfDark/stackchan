cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Relax GCC 14 strictness for warnings that fire only in vendored
# third-party libraries (libpeer, sepfy/srtp). These are all benign:
#   * discarded-qualifiers in libpeer's dtls_srtp.c — fixed locally,
#     also relaxed globally so any other instance doesn't bite us
#   * incompatible-pointer-types in sepfy/srtp — uint32_t vs unsigned int
#     pointer mismatch (same size on this platform, just GCC's strict
#     type-checking refusing to coerce). Patching the library would mean
#     diverging from upstream sepfy/srtp; relaxing the warning is the
#     pragmatic call.
# Done at project-scope (before any component) so it reaches every
# target the build creates. Our own sources still build at -Werror.
idf_build_set_property(COMPILE_OPTIONS
    "-Wno-error=incompatible-pointer-types;-Wno-error=discarded-qualifiers"
    APPEND)

project(pipecat-voice-firmware)
