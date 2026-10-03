#include "domain/energy_gate.hpp"

namespace domain {

uint32_t rms_i16(const int16_t* samples, std::size_t n)
{
    if (n == 0 || !samples) return 0;
    uint64_t sumsq = 0;
    for (std::size_t i = 0; i < n; ++i) {
        const int32_t s = samples[i];
        sumsq += static_cast<uint64_t>(s * s);
    }
    const uint32_t meansq = static_cast<uint32_t>(sumsq / n);
    if (meansq == 0) return 0;
    // Integer Newton's method, 8 iterations — converges well past our
    // LED-gate precision needs for the 32-bit input range.
    uint32_t guess = meansq > 65'536 ? 256 : 16;
    for (int i = 0; i < 8; ++i) {
        guess = (guess + meansq / guess) / 2;
    }
    return guess;
}

int32_t peak_abs_i16(const int16_t* samples, std::size_t n)
{
    if (n == 0 || !samples) return 0;
    int32_t peak = 0;
    for (std::size_t i = 0; i < n; ++i) {
        int32_t a = samples[i];
        if (a < 0) a = -a;
        if (a > peak) peak = a;
    }
    return peak;
}

} // namespace domain
