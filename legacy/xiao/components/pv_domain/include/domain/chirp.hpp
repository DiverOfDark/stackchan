#pragma once

// Local UI sound effects — short synthetic "chirps" played on the device
// speaker (no backend involved) to mark events: wake-word recognised and
// end-of-session.
//
// Style: "evil cyberpunk corporate" — a cold, clinical megacorp-terminal feel.
// A low ominous DRONE bed carries the menace; over it a clean, glassy FM-bell
// chime (fundamental + octave + a metallic inharmonic partial + a faint detune)
// states a cold TRITONE (the dissonant "devil's interval") — polished, not
// gritty. A smooth (non-percussive) attack keeps it clinical rather than
// playful. Wake rises to the tritone ("access granted, watching you"); End
// descends and settles ("session terminated"). 16 kHz mono int16.
//
// Pure, header-only, host-testable.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace domain {

enum class Chirp : uint8_t { Wake, End };

// Upper bound on samples a chirp can produce (~875 ms at 16 kHz).
inline constexpr std::size_t kChirpMaxSamples = 14000;

// Synthesize `kind` into `out` (capacity `cap` samples). Returns samples
// written. Notes (incl. a low drone, at reduced gain) are summed at their
// onsets so they overlap; each is a clean glassy bell with a smooth attack and
// a long decay, ramped to zero at the very end so it can't click.
inline std::size_t synth_chirp(Chirp kind, int16_t* out, std::size_t cap)
{
    constexpr double kFs = 16000.0;

    struct Note { double freq, onset_ms, ring_ms, gain; };
    // D3 146.83 (drone), D4 293.66, Ab4 415.30 (a tritone above D — cold and
    // dissonant). The drone runs the whole length as the menacing bed.
    const Note wake[] = {
        {146.83,   0.0, 820.0, 0.40},   // low drone bed
        {293.66, 100.0, 380.0, 1.00},   // D4
        {415.30, 280.0, 540.0, 1.00},   // Ab4 — tritone (cold dissonance)
    };
    const Note end_[] = {
        {146.83,   0.0, 820.0, 0.40},   // low drone bed
        {415.30, 100.0, 380.0, 1.00},   // Ab4 — tritone
        {293.66, 280.0, 540.0, 1.00},   // D4 — settles
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
        const double gain  = ns[k].gain;
        const double atk_n = kFs * 0.012;     // 12 ms smooth attack (clinical, not plucky)
        const double tau   = ring * 0.42;     // long decay → cold sustain
        for (std::size_t i = 0; i < ring && onset + i < total; ++i) {
            const double a   = (i < atk_n) ? (i / atk_n) : 1.0;
            const double env = a * std::exp(-static_cast<double>(i) / tau);
            const double v   = 0.50 * std::sin(2.0 * M_PI * f         * i / kFs)   // fundamental
                             + 0.28 * std::sin(2.0 * M_PI * f * 2.0   * i / kFs)   // octave (body)
                             + 0.14 * std::sin(2.0 * M_PI * f * 2.76  * i / kFs)   // metallic FM partial (glass)
                             + 0.08 * std::sin(2.0 * M_PI * f * 1.004 * i / kFs);  // faint detune (synthetic)
            int s = out[onset + i] + static_cast<int>(gain * env * v * 6000.0);
            if (s > 32767) s = 32767; else if (s < -32768) s = -32768;
            out[onset + i] = static_cast<int16_t>(s);
        }
    }

    // Ramp the last few ms to zero so the sustain can't click when playback
    // stops. i counts back from the end: last sample (i=0) → 0.
    const std::size_t fade = std::min<std::size_t>(96, total);   // ~6 ms
    for (std::size_t i = 0; i < fade; ++i) {
        const double g = static_cast<double>(i) / fade;
        out[total - 1 - i] = static_cast<int16_t>(out[total - 1 - i] * g);
    }
    return total;
}

} // namespace domain
