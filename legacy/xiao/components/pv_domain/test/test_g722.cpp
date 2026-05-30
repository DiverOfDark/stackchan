#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "domain/g722.hpp"

using namespace domain;

namespace {

// 16 kHz sine of `n` samples.
std::vector<int16_t> sine(double freq_hz, int n, double amp = 8000.0) {
    std::vector<int16_t> v(n);
    for (int i = 0; i < n; ++i)
        v[i] = static_cast<int16_t>(amp * std::sin(2.0 * M_PI * freq_hz * i / 16000.0));
    return v;
}

double rms(const int16_t* x, int from, int to) {
    double acc = 0;
    for (int i = from; i < to; ++i) acc += static_cast<double>(x[i]) * x[i];
    return std::sqrt(acc / (to - from));
}

// Best normalised cross-correlation searching small lags (the QMF + ADPCM
// path introduces a group delay, so the decoded tone is shifted).
double best_xcorr(const std::vector<int16_t>& a, const std::vector<int16_t>& b, int max_lag) {
    const int n = static_cast<int>(a.size());
    double best = -2.0;
    for (int lag = 0; lag <= max_lag; ++lag) {
        double num = 0, da = 0, db = 0;
        for (int i = 64; i + lag < n; ++i) {
            num += static_cast<double>(a[i]) * b[i + lag];
            da  += static_cast<double>(a[i]) * a[i];
            db  += static_cast<double>(b[i + lag]) * b[i + lag];
        }
        if (da > 0 && db > 0) best = std::max(best, num / std::sqrt(da * db));
    }
    return best;
}

} // namespace

TEST_CASE("g722 encode produces half as many bytes", "[g722]") {
    G722Codec enc; g722_init(enc);
    auto pcm = sine(1000.0, 320);
    std::vector<uint8_t> out(160, 0);
    REQUIRE(g722_encode(enc, pcm.data(), pcm.size(), out.data()) == 160);
}

TEST_CASE("g722 decode produces twice as many samples", "[g722]") {
    G722Codec enc, dec; g722_init(enc); g722_init(dec);
    auto pcm = sine(1000.0, 320);
    std::vector<uint8_t> bytes(160, 0);
    std::vector<int16_t> back(320, 0);
    std::size_t nb = g722_encode(enc, pcm.data(), pcm.size(), bytes.data());
    REQUIRE(g722_decode(dec, bytes.data(), nb, back.data()) == 320);
}

TEST_CASE("g722 round-trip recovers a 1 kHz tone (correlation + level)", "[g722]") {
    G722Codec enc, dec; g722_init(enc); g722_init(dec);
    const int n = 1600;                       // 100 ms
    auto pcm = sine(1000.0, n);
    std::vector<uint8_t> bytes(n / 2, 0);
    std::vector<int16_t> back(n, 0);
    std::size_t nb = g722_encode(enc, pcm.data(), pcm.size(), bytes.data());
    g722_decode(dec, bytes.data(), nb, back.data());

    // Decoded waveform must strongly correlate with the input (a garbled
    // table/scaling bug would destroy this).
    REQUIRE(best_xcorr(pcm, back, 40) > 0.8);
    // And energy must be preserved to within a modest margin.
    const double ri = rms(pcm.data(), 64, n);
    const double ro = rms(back.data(), 64, n);
    REQUIRE(ro > 0.5 * ri);
    REQUIRE(ro < 1.5 * ri);
}

TEST_CASE("g722 round-trip recovers a 6 kHz (high-band) tone", "[g722]") {
    G722Codec enc, dec; g722_init(enc); g722_init(dec);
    const int n = 1600;
    auto pcm = sine(6000.0, n);
    std::vector<uint8_t> bytes(n / 2, 0);
    std::vector<int16_t> back(n, 0);
    std::size_t nb = g722_encode(enc, pcm.data(), pcm.size(), bytes.data());
    g722_decode(dec, bytes.data(), nb, back.data());
    // High band is only 2-bit ADPCM, so be looser, but it must still track.
    REQUIRE(best_xcorr(pcm, back, 40) > 0.5);
}

TEST_CASE("g722 silence stays quiet", "[g722]") {
    G722Codec enc, dec; g722_init(enc); g722_init(dec);
    std::vector<int16_t> zeros(640, 0);
    std::vector<uint8_t> bytes(320, 0);
    std::vector<int16_t> back(640, 0);
    std::size_t nb = g722_encode(enc, zeros.data(), zeros.size(), bytes.data());
    g722_decode(dec, bytes.data(), nb, back.data());
    REQUIRE(rms(back.data(), 0, 640) < 64.0);
}

TEST_CASE("g722 encoder is deterministic from a fresh state", "[g722]") {
    auto pcm = sine(1500.0, 640);
    std::vector<uint8_t> a(320, 0), b(320, 1);
    G722Codec e1; g722_init(e1); g722_encode(e1, pcm.data(), pcm.size(), a.data());
    G722Codec e2; g722_init(e2); g722_encode(e2, pcm.data(), pcm.size(), b.data());
    REQUIRE(a == b);
}
