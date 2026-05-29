cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Relax GCC 14 strictness for warnings that fire only in vendored
# third-party libraries (libpeer, sepfy/srtp, esphome/micro-opus). These
# are all benign:
#   * discarded-qualifiers in libpeer's dtls_srtp.c — fixed locally,
#     also relaxed globally so any other instance doesn't bite us
#   * incompatible-pointer-types in sepfy/srtp — uint32_t vs unsigned int
#     pointer mismatch (same size on this platform, just GCC's strict
#     type-checking refusing to coerce). Patching the library would mean
#     diverging from upstream sepfy/srtp; relaxing the warning is the
#     pragmatic call.
#   * maybe-uninitialized in opus silk/NLSF2A.c (cos_LSF_QA) — GCC 14
#     false positive on an unreachable path when silk_NLSF2A_find_poly is
#     inlined. Must be set here, NOT via platformio.ini build_flags: those
#     don't reach the managed esphome__micro-opus component, and its own
#     -Werror would re-promote the warning anyway. APPEND puts our
#     -Wno-error AFTER the component's -Werror so it wins.
# Done at project-scope (before any component) so it reaches every
# target the build creates, managed components included.
idf_build_set_property(COMPILE_OPTIONS
    "-Wno-error=incompatible-pointer-types;-Wno-error=discarded-qualifiers;-Wno-error=maybe-uninitialized"
    APPEND)

project(pipecat-voice-firmware)
