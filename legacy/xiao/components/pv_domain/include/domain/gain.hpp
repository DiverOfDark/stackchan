#pragma once

// Mic input leveling. Pure, header-only, host-testable — no ESP-IDF deps.
//
// The XVF3800 hands us 32-bit (Q1.31, MSB-aligned) I2S samples whose level
// depends entirely on the chip's flashed gain/AGC defaults; in practice the
// far-field signal lands well below full scale, so both the wake-word model
// and backend STT need it brought up. The old capture code did this with a
// raw arithmetic shift plus a HARD clamp:
//
//     int32_t boosted = raw >> 13;            // +18 dB
//     if (boosted > INT16_MAX) boosted = INT16_MAX;   // square-wave clip
//
// Hard clipping turns loud speech into a square wave, which (a) wrecks the
// log-mel features the wake model was trained on and (b) makes backend
// Whisper transcribe garbage. scale_to_i16() replaces that with a linear
// region up to `knee` and a smooth tanh saturation above it, so normal
// speech passes through untouched and only peaks get gently compressed.

#include <cmath>
#include <cstdint>

namespace domain {

// Convert a Q1.31 32-bit sample to int16 with linear `gain` applied and a
// soft-knee limiter. `gain == 1.0` is exactly the conventional `raw >> 16`
// reduction; `gain == 8.0` is the +18 dB the wake model expects. Below
// `knee` (as a fraction of full scale) the response is linear; above it it
// eases asymptotically toward full scale and never hard-clips.
inline int16_t scale_to_i16(int32_t raw_q31, float gain, float knee = 0.75f)
{
    // Q1.31 -> [-1, 1).
    float x = static_cast<float>(raw_q31) * (1.0f / 2147483648.0f);
    x *= gain;

    const float a = x < 0.0f ? -x : x;
    float y;
    if (a <= knee) {
        y = x;                                   // linear region
    } else {
        // Smoothly compress everything above the knee into [knee, 1).
        // Slope matches the linear region at the knee (C1-continuous).
        const float over = (a - knee) / (1.0f - knee);
        const float comp = knee + (1.0f - knee) * std::tanh(over);
        y = x < 0.0f ? -comp : comp;
    }
    if (y >  1.0f) y =  1.0f;
    if (y < -1.0f) y = -1.0f;

    long v = std::lroundf(y * 32767.0f);
    if (v >  32767) v =  32767;
    if (v < -32768) v = -32768;
    return static_cast<int16_t>(v);
}

} // namespace domain
