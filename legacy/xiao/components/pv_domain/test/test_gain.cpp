#include <catch2/catch_test_macros.hpp>

#include <cstdint>

#include "domain/gain.hpp"

using domain::scale_to_i16;

TEST_CASE("scale_to_i16 maps zero to zero", "[gain]") {
    REQUIRE(scale_to_i16(0, 1.0f) == 0);
    REQUIRE(scale_to_i16(0, 8.0f) == 0);
}

TEST_CASE("gain 1.0 reproduces the old >>16 reduction in the linear region", "[gain]") {
    // raw = 1000 << 16 is what an int16 value of 1000 looks like MSB-aligned
    // in a 32-bit slot. >>16 gave exactly 1000; scale_to_i16(...,1.0) matches.
    REQUIRE(scale_to_i16(1000 << 16, 1.0f) == 1000);
    REQUIRE(scale_to_i16(-(1000 << 16), 1.0f) == -1000);
}

TEST_CASE("small signals are scaled linearly by the gain", "[gain]") {
    // normalized 0.05 * gain 4 = 0.20 -> 0.20 * 32767 ~= 6553.
    const int32_t raw = static_cast<int32_t>(0.05f * 2147483648.0f);
    const int16_t out = scale_to_i16(raw, 4.0f);
    REQUIRE(out >= 6540);
    REQUIRE(out <= 6566);
}

TEST_CASE("loud input soft-limits instead of hard-clipping", "[gain]") {
    // A near-full-scale input at +18 dB is driven up close to full scale but
    // saturates smoothly (the int16 type guarantees it can't exceed 32767).
    REQUIRE(scale_to_i16(INT32_MAX, 8.0f) > 32000);
    REQUIRE(scale_to_i16(INT32_MIN, 8.0f) < -32000);
}

TEST_CASE("response is monotonic across the knee", "[gain]") {
    int16_t prev = INT16_MIN;
    for (double n = 0.0; n < 1.0; n += 0.01) {
        const int32_t raw = static_cast<int32_t>(n * 2147483647.0);
        const int16_t out = scale_to_i16(raw, 8.0f);
        REQUIRE(out >= prev);
        prev = out;
    }
}

TEST_CASE("output is sign-symmetric", "[gain]") {
    for (double n = 0.05; n < 1.0; n += 0.1) {
        const int32_t raw = static_cast<int32_t>(n * 2147483647.0);
        REQUIRE(scale_to_i16(raw, 4.0f) == -scale_to_i16(-raw, 4.0f));
    }
}
