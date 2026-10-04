#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <vector>

#include "domain/g711.hpp"

using namespace domain;

TEST_CASE("ulaw known reference points", "[g711]") {
    REQUIRE(ulaw_encode(0) == 0xFF);   // zero encodes to 0xFF
    REQUIRE(ulaw_decode(0xFF) == 0);   // and back to ~0
}

TEST_CASE("ulaw round-trip preserves sign and is monotonic", "[g711]") {
    int16_t prev = -32768;
    for (int x = -32000; x <= 32000; x += 250) {
        int16_t r = ulaw_decode(ulaw_encode(static_cast<int16_t>(x)));
        if (x > 0) REQUIRE(r >= 0);
        if (x < -8) REQUIRE(r <= 0);
        REQUIRE(r >= prev);            // non-decreasing across the sweep
        prev = r;
    }
}

TEST_CASE("ulaw round-trip error stays small relative to magnitude", "[g711]") {
    // µ-law is logarithmic: absolute error grows with magnitude but stays a
    // small fraction of the sample. Loose bound that still catches gross bugs.
    for (int x = -32000; x <= 32000; x += 137) {
        int16_t r = ulaw_decode(ulaw_encode(static_cast<int16_t>(x)));
        int err = std::abs(r - x);
        REQUIRE(err <= std::abs(x) / 8 + 256);
    }
}

TEST_CASE("ulaw is sign-symmetric", "[g711]") {
    for (int x = 1; x <= 32000; x += 311) {
        int16_t pos = ulaw_decode(ulaw_encode(static_cast<int16_t>(x)));
        int16_t neg = ulaw_decode(ulaw_encode(static_cast<int16_t>(-x)));
        REQUIRE(pos == -neg);
    }
}

TEST_CASE("downsample halves length and preserves a constant", "[g711]") {
    std::vector<int16_t> in(320, 1234);
    std::vector<int16_t> out(160, 0);
    downsample_2x(in.data(), in.size(), out.data());
    for (int16_t s : out) REQUIRE(s == 1234);
}

TEST_CASE("upsample doubles length and preserves a constant", "[g711]") {
    std::vector<int16_t> in(160, -2000);
    std::vector<int16_t> out(320, 0);
    upsample_2x(in.data(), in.size(), out.data());
    for (int16_t s : out) REQUIRE(s == -2000);
}

TEST_CASE("g711_encode_16k produces half the bytes", "[g711]") {
    std::vector<int16_t> pcm(320, 500);
    std::vector<uint8_t> ulaw(160, 0);
    REQUIRE(g711_encode_16k(pcm.data(), pcm.size(), ulaw.data()) == 160);
    // every byte identical for a constant input
    for (std::size_t i = 1; i < ulaw.size(); ++i) REQUIRE(ulaw[i] == ulaw[0]);
}

TEST_CASE("g711 16k encode→decode round-trips near the original level", "[g711]") {
    std::vector<int16_t> pcm(320, 4000);
    std::vector<uint8_t> ulaw(160, 0);
    std::vector<int16_t> back(320, 0);
    std::size_t nb = g711_encode_16k(pcm.data(), pcm.size(), ulaw.data());
    std::size_t ns = g711_decode_to_16k(ulaw.data(), nb, back.data());
    REQUIRE(ns == 320);
    // companding loss only; level should be within a few percent.
    for (int16_t s : back) REQUIRE(std::abs(s - 4000) <= 4000 / 8 + 64);
}
