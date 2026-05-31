#pragma once

// Local UI sound effects — short synthetic "chirps" played on the device
// speaker (no backend involved) to mark events: wake-word recognised and
// end-of-session.
//
// Style: an ORIGINAL synthesis with a dark, menacing cyberpunk flavour — low
// register, the tritone (the "diabolus in musica" — the classic ominous
// interval), a sub-octave for weight and a gritty odd harmonic, with a slight
// detune so it sits a hair out of tune (unsettling). Notes overlap and ring
// into each other. Wake = an ominous rise to the tritone ("a presence wakes");
// End = a descent into the depths ("powering down"). 16 kHz mono int16 — the
// device's native playback rate, so the playback task writes it straight to
// I2S. (Low fundamentals roll off on the small speaker, but the odd harmonics
// keep the dark pitch audible via the missing-fundamental effect.)
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
// written. Each note = fundamental + sub-octave (weight) + a detuned partial
// (unsettling beat) + an odd harmonic (gritty edge), shaped by a fast attack
// and a long-ish exponential decay; notes are summed at their onsets so their
// tails overlap into a brooding arpeggio.
inline std::size_t synth_chirp(Chirp kind, int16_t* out, std::size_t cap)
{
    constexpr double kFs = 16000.0;

    struct Note { double freq, onset_ms, ring_ms; };
    // Low register around A. The Eb is a tritone above A — the dissonant,
    // menacing interval.  A3 220, Eb4 311.13, D4 293.66, E3 164.81.
    const Note wake[] = {
        {220.00,   0.0, 460.0}, {311.13, 140.0, 500.0}, {293.66, 300.0, 560.0},
    };
    const Note end_[] = {
        {311.13,   0.0, 420.0}, {220.00, 140.0, 440.0}, {164.81, 290.0, 580.0},
    };
    const Note* ns = (kind == Chirp::Wake) ? wake : end_;
    const int   nn = 3;

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
        const double atk_n = kFs * 0.005;     // 5 ms click-free attack
        const double tau   = ring * 0.45;     // long-ish decay → ominous sustain
        for (std::size_t i = 0; i < ring && onset + i < total; ++i) {
            const double a   = (i < atk_n) ? (i / atk_n) : 1.0;
            const double env = a * std::exp(-static_cast<double>(i) / tau);
            const double v   = 0.45 * std::sin(2.0 * M_PI * f          * i / kFs)   // fundamental
                             + 0.28 * std::sin(2.0 * M_PI * f * 0.5    * i / kFs)   // sub-octave (weight)
                             + 0.17 * std::sin(2.0 * M_PI * f * 1.004  * i / kFs)   // detune (unsettling)
                             + 0.10 * std::sin(2.0 * M_PI * f * 3.0    * i / kFs);  // odd harmonic (grit)
            int s = out[onset + i] + static_cast<int>(env * v * 6500.0);
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            out[onset + i] = static_cast<int16_t>(s);
        }
    }

    // The long decay can still be ringing at the buffer end — ramp the last few
    // ms to zero so there's no click when playback stops.
    const std::size_t fade = std::min<std::size_t>(96, total);   // ~6 ms
    for (std::size_t i = 0; i < fade; ++i) {
        // i counts back from the end: the very last sample (i=0) → 0, ramping
        // back up to ~1 at `fade` samples in.
        const double g = static_cast<double>(i) / fade;
        out[total - 1 - i] = static_cast<int16_t>(out[total - 1 - i] * g);
    }
    return total;
}

} // namespace domain
