# Host-side test runner for the firmware's domain layer.
#
# Built with plain gcc/clang on the developer's laptop — NOT cross-compiled
# for ESP32. The point is to unit-test the pure logic (state machines,
# parsers, energy gates) without flashing.
#
# Usage:
#     mkdir -p host_test/build && cd host_test/build
#     cmake .. && cmake --build . && ctest --output-on-failure
#
# CI would run `cmake -S host_test -B host_test/build && cmake --build host_test/build && ctest`.

cmake_minimum_required(VERSION 3.16)
project(pipecat-firmware-host-tests CXX)

set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Werror)

# Catch2 v3 fetched on first configure. ~10 MB clone; small for a test
# runner, and avoids embedding a vendored copy of the library in the
# repo. CI caches the build dir, so the fetch happens once.
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.4
)
FetchContent_MakeAvailable(Catch2)

# Domain headers live at firmware/components/domain/include — pulled in
# directly without going through the ESP-IDF build system.
set(DOMAIN_DIR "${CMAKE_SOURCE_DIR}/../components/pv_domain")
include_directories(${DOMAIN_DIR}/include)

# Test sources land here in Phase B. Each test_xxx.cpp is one Catch2 test
# binary registered with CTest. Sources come from domain/src/ (so the
# same translation units that ship in firmware are what's under test).
set(DOMAIN_TEST_TARGETS
    # Phase B adds e.g.:
    #   test_led_fsm     domain/src/led_fsm.cpp        test/test_led_fsm.cpp
    #   test_session_fsm domain/src/session_fsm.cpp    test/test_session_fsm.cpp
    #   ...
)

enable_testing()

# Macro: declare a test binary that links Catch2 + given sources and gets
# registered as a CTest case.
function(add_domain_test name)
    add_executable(${name} ${ARGN})
    target_link_libraries(${name} PRIVATE Catch2::Catch2WithMain)
    add_test(NAME ${name} COMMAND ${name})
endfunction()

add_domain_test(test_led_fsm
    ${DOMAIN_DIR}/src/led_fsm.cpp
    ${DOMAIN_DIR}/test/test_led_fsm.cpp)

add_domain_test(test_wake_window
    ${DOMAIN_DIR}/src/wake_window.cpp
    ${DOMAIN_DIR}/test/test_wake_window.cpp)

add_domain_test(test_energy_gate
    ${DOMAIN_DIR}/src/energy_gate.cpp
    ${DOMAIN_DIR}/test/test_energy_gate.cpp)

add_domain_test(test_form_urlencoded
    ${DOMAIN_DIR}/src/form_urlencoded.cpp
    ${DOMAIN_DIR}/test/test_form_urlencoded.cpp)

add_domain_test(test_session_fsm
    ${DOMAIN_DIR}/src/session_fsm.cpp
    ${DOMAIN_DIR}/test/test_session_fsm.cpp)

# gain.hpp is header-only — no src/ translation unit to compile in.
add_domain_test(test_gain
    ${DOMAIN_DIR}/test/test_gain.cpp)
