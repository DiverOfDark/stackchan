#pragma once

// Local UI sound effects — short synthetic "chirps" played on the device
// speaker (no backend involved) to mark events: wake-word recognised and
// end-of-session.
//
// Style: an ORIGINAL synthesis evoking the Cyberpunk-2077 holo-call ring (a
// synthetic, slightly melancholic minor arpeggio) — NOT the game's copyrighted
// audio asset. Notes overlap and ring into each other with a detuned bell/pluck
// timbre. Wake = ascending ("incoming / connecting"); End = descending ("call
// ended"). 16 kHz mono int16 — the device's native playback rate, so the
// playback task writes it straight to I2S.
//
// Pure, header-only, host-testable.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace domain {

enum class Chirp : uint8_t { Wake, End };

// Upper bound on samples a chirp can produce (~875 ms at 16 kHz), so callers
// can size a buffer.
inline constexpr std::size_t kChirpMaxSamples = 14000;

// Synthesize `kind` into `out` (capacity `cap` samples). Returns samples
// written. Each note = fundamental + a slight detune (shimmer) + an octave
// (bell), shaped by a 4 ms attack and exponential decay; notes are summed at
// their onsets so their tails overlap into a ringing arpeggio.
inline std::size_t synth_chirp(Chirp kind, int16_t* out, std::size_t cap)
{
    constexpr double kFs = 16000.0;

    struct Note { double freq, onset_ms, ring_ms; };
    // A-natural-minor flavour (A4 440, C5 523.25, E5 659.25, A5 880).
    const Note wake[] = {
        {440.00,   0.0, 340.0}, {523.25, 120.0, 340.0},
        {659.25, 240.0, 340.0}, {880.00, 360.0, 480.0},
    };
    const Note end_[] = {
        {880.00,   0.0, 320.0}, {659.25, 120.0, 320.0}, {440.00, 240.0, 520.0},
    };
    const Note* ns = (kind == Chirp::Wake) ? wake : end_;
    const int   nn = (kind == Chirp::Wake) ? 4 : 3;

    double total_ms = 0.0;
    for (int i = 0; i < nn; ++i)
        total_ms = std::fmax(total_ms, ns[i].onset_ms + ns[i].ring_ms);
    const std::size_t total =
        std::min(cap, static_cast<std::size_t>(total_ms * kFs / 1000.0));

    for (std::size_t i = 0; i < total; ++i) out[i] = 0;

    for (int k = 0; k < nn; ++k) {
        const std::size_t onset = static_cast<std::size_t>(ns[k].onset_ms * kFs / 1000.0);
        const std::size_t ring  = static_cast<std::size_t>(ns[k].ring_ms  * kFs / 1000.0);
        const double f     = ns[k].freq;
        const double atk_n = kFs * 0.004;     // 4 ms click-free attack
        const double tau   = ring * 0.32;     // decay time constant (samples)
        for (std::size_t i = 0; i < ring && onset + i < total; ++i) {
            const double a   = (i < atk_n) ? (i / atk_n) : 1.0;
            const double env = a * std::exp(-static_cast<double>(i) / tau);
            const double v   = 0.55 * std::sin(2.0 * M_PI * f         * i / kFs)   // fundamental
                             + 0.25 * std::sin(2.0 * M_PI * f * 1.002 * i / kFs)   // detune shimmer
                             + 0.20 * std::sin(2.0 * M_PI * f * 2.0   * i / kFs);  // octave (bell)
            int s = out[onset + i] + static_cast<int>(env * v * 6500.0);
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            out[onset + i] = static_cast<int16_t>(s);
        }
    }
    return total;
}

} // namespace domain
