#pragma once

// Local UI sound effects — short synthetic "chirps" played on the device
// speaker (no backend involved) to mark events: wake-word recognised and
// end-of-session. Cyberpunk-2077 flavour: bright, synthetic, slightly metallic
// two-note blips with a fast attack and exponential decay. Wake = ascending
// ("online"); End = descending ("offline").
//
// Pure, header-only, host-testable. 16 kHz mono int16 — the device's native
// playback rate, so the playback task can write it straight to I2S.

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace domain {

enum class Chirp : uint8_t { Wake, End };

// Upper bound on samples a chirp can produce, so callers can size a static
// buffer. ~225 ms at 16 kHz.
inline constexpr std::size_t kChirpMaxSamples = 3600;

// Synthesize `kind` into `out` (capacity `cap` samples). Returns the number of
// samples written. Each chirp is two frequency-swept blips; each blip is a
// fundamental plus a fifth above (×1.5) for a synthetic ring, shaped by a fast
// attack + exponential decay so there are no clicks.
inline std::size_t synth_chirp(Chirp kind, int16_t* out, std::size_t cap)
{
    constexpr double kFs   = 16000.0;
    constexpr double kAmp  = 9000.0;   // ~27% full scale — audible, not jarring

    struct Seg { double f0, f1, dur_ms, gap_ms; };
    // Two blips per chirp. Wake rises (activation); End falls (shutdown).
    const Seg wake[2] = {{660.0,  990.0, 70.0, 25.0}, { 990.0, 1560.0, 90.0, 0.0}};
    const Seg end_[2] = {{1480.0, 990.0, 80.0, 25.0}, { 760.0,  480.0, 110.0, 0.0}};
    const Seg* segs = (kind == Chirp::Wake) ? wake : end_;

    std::size_t n = 0;
    double phase = 0.0, phase5 = 0.0;
    for (int s = 0; s < 2; ++s) {
        const Seg& seg = segs[s];
        const auto samples = static_cast<std::size_t>(seg.dur_ms * kFs / 1000.0);
        const auto gap     = static_cast<std::size_t>(seg.gap_ms * kFs / 1000.0);
        const double atk_n = samples * 0.06;   // ~attack length in samples
        for (std::size_t i = 0; i < samples && n < cap; ++i, ++n) {
            const double t = static_cast<double>(i) / samples;     // 0..1 in segment
            const double f = seg.f0 + (seg.f1 - seg.f0) * t;       // linear sweep
            phase  += 2.0 * M_PI * f         / kFs;
            phase5 += 2.0 * M_PI * (f * 1.5) / kFs;
            const double atk = (i < atk_n) ? (i / atk_n) : 1.0;    // click-free attack
            const double env = atk * std::exp(-3.0 * t);           // decay over the blip
            const double v   = 0.6 * std::sin(phase) + 0.4 * std::sin(phase5);
            out[n] = static_cast<int16_t>(env * v * kAmp);
        }
        for (std::size_t i = 0; i < gap && n < cap; ++i, ++n) out[n] = 0;
    }
    return n;
}

} // namespace domain
