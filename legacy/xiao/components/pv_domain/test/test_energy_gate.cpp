#include <catch2/catch_test_macros.hpp>

#include <vector>

#include "domain/energy_gate.hpp"

using namespace domain;

TEST_CASE("rms_i16 empty / null input returns 0", "[energy_gate]") {
    REQUIRE(rms_i16(nullptr, 0) == 0);
    REQUIRE(rms_i16(nullptr, 10) == 0);
    int16_t one[1] = {0};
    REQUIRE(rms_i16(one, 0) == 0);
}

TEST_CASE("rms_i16 of pure silence is 0", "[energy_gate]") {
    std::vector<int16_t> v(320, 0);
    REQUIRE(rms_i16(v.data(), v.size()) == 0);
}

TEST_CASE("rms_i16 of constant signal equals the constant", "[energy_gate]") {
    std::vector<int16_t> v(320, 1000);
    // RMS of a constant N-vector is exactly N — within 1 LSB rounding.
    auto r = rms_i16(v.data(), v.size());
    REQUIRE(r >= 999);
    REQUIRE(r <= 1001);
}

TEST_CASE("rms_i16 of alternating square wave matches amplitude", "[energy_gate]") {
    std::vector<int16_t> v(320);
    for (size_t i = 0; i < v.size(); ++i) v[i] = (i & 1) ? 2000 : -2000;
    auto r = rms_i16(v.data(), v.size());
    REQUIRE(r >= 1999);
    REQUIRE(r <= 2001);
}

TEST_CASE("peak_abs_i16 finds the maximum magnitude", "[energy_gate]") {
    int16_t v[] = {100, -300, 50, 200, -150};
    REQUIRE(peak_abs_i16(v, 5) == 300);
}

TEST_CASE("peak_abs_i16 handles INT16_MIN without UB", "[energy_gate]") {
    // -32768 has no positive twin in int16; the implementation casts to
    // int32 before negating so this is well-defined.
    int16_t v[] = {-32768, 0, 100};
    REQUIRE(peak_abs_i16(v, 3) == 32768);
}

TEST_CASE("peak_abs_i16 empty / null returns 0", "[energy_gate]") {
    REQUIRE(peak_abs_i16(nullptr, 0) == 0);
    REQUIRE(peak_abs_i16(nullptr, 5) == 0);
    int16_t one[1] = {0};
    REQUIRE(peak_abs_i16(one, 0) == 0);
}
