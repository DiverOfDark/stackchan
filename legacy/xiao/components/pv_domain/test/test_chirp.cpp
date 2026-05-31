#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "domain/chirp.hpp"

using namespace domain;

static long peak(const int16_t* x, std::size_t n) {
    long p = 0;
    for (std::size_t i = 0; i < n; ++i) p = std::max(p, std::labs(x[i]));
    return p;
}

TEST_CASE("chirp produces a bounded, non-empty, non-silent waveform", "[chirp]") {
    for (Chirp k : {Chirp::Wake, Chirp::End}) {
        std::vector<int16_t> buf(kChirpMaxSamples, 32767);
        std::size_t n = synth_chirp(k, buf.data(), buf.size());
        REQUIRE(n > 16000 * 0.10);              // at least ~100 ms of audio
        REQUIRE(n <= kChirpMaxSamples);
        const long p = peak(buf.data(), n);
        REQUIRE(p > 1000);                      // audible
        REQUIRE(p <= 32767);                    // never clips int16
    }
}

TEST_CASE("chirp is click-free at the start", "[chirp]") {
    // The attack ramp means the first sample is at/near zero — no DAC pop.
    std::vector<int16_t> buf(kChirpMaxSamples, 12345);
    synth_chirp(Chirp::Wake, buf.data(), buf.size());
    REQUIRE(std::labs(buf[0]) < 200);
}

TEST_CASE("chirp respects the output capacity", "[chirp]") {
    std::vector<int16_t> small(64, 0);
    std::size_t n = synth_chirp(Chirp::End, small.data(), small.size());
    REQUIRE(n <= small.size());
}
