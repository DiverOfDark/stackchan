# Domain layer — pure C++17, no ESP-IDF dependencies.
#
# These files are the same source used by both the firmware (compiled here
# via idf_component_register) and the host-side test runner (compiled via
# firmware/host_test/CMakeLists.txt with plain gcc/clang + Catch2).
#
# Keep this REQUIRES list empty. If a domain header ever needs an ESP-IDF
# type, it does not belong in domain/ — move it to hal/ or transport/.

set(DOMAIN_SRCS
    "src/led_fsm.cpp"
    "src/wake_window.cpp"
    "src/energy_gate.cpp"
    "src/form_urlencoded.cpp"
    "src/session_fsm.cpp"
)

# idf_component_register requires either SRCS or INCLUDE_DIRS to be non-
# empty. We always have INCLUDE_DIRS, so empty SRCS is fine pre-Phase-B.
idf_component_register(
    SRCS ${DOMAIN_SRCS}
    INCLUDE_DIRS "include"
)

if(DOMAIN_SRCS)
    # Pure C++17 — explicit so a stray .c file accidentally added here
    # doesn't silently slip in.
    set_source_files_properties(${DOMAIN_SRCS} PROPERTIES
        COMPILE_FLAGS "-std=gnu++17 -Wall -Wextra -Werror"
    )
endif()
