#pragma once

// G.711 µ-law codec + 2× resampling. Pure, header-only, host-testable.
//
// Why G.711 instead of Opus on this device: Opus encode/decode needs a large
// (>120 KB) scratch "pseudostack" that, on this board, only fits in PSRAM —
// and SILK hammers it every frame, making opus_encode ~1.5× real time. That
// stalled the capture task, so the mic audio feeding the wake word and the STT
// uplink was continuously dropped. G.711 companding is a branch-free table-ish
// computation with zero scratch, so encode/decode are effectively free and the
// capture pipeline runs in real time. Trade-off: 8 kHz narrowband on the wire.
//
// The wire is 8 kHz (WebRTC PCMU is defined at 8 kHz); the mic/I2S path stays
// 16 kHz. We down/upsample at the codec boundary. The local wake word still
// sees the full 16 kHz mic path and is unaffected by this codec.

#include <cstddef>
#include <cstdint>

namespace domain {

// ---- µ-law companding (ITU-T G.711, classic Sun reference) ---------------

inline uint8_t ulaw_encode(int16_t pcm16)
{
    constexpr int32_t kBias = 0x84;       // 132
    constexpr int32_t kClip = 32635;
    int32_t pcm = pcm16;
    int sign = 0;
    if (pcm < 0) { pcm = -pcm; sign = 0x80; }   // int32 math: INT16_MIN is safe
    if (pcm > kClip) pcm = kClip;
    pcm += kBias;
    int exponent = 7;
    for (int mask = 0x4000; (pcm & mask) == 0 && exponent > 0; --exponent, mask >>= 1) {}
    const int mantissa = (pcm >> (exponent + 3)) & 0x0F;
    return static_cast<uint8_t>(~(sign | (exponent << 4) | mantissa));
}

inline int16_t ulaw_decode(uint8_t u)
{
    u = static_cast<uint8_t>(~u);
    int t = ((u & 0x0F) << 3) + 0x84;
    t <<= (u & 0x70) >> 4;
    return static_cast<int16_t>((u & 0x80) ? (0x84 - t) : (t - 0x84));
}

// ---- 2× resampling -------------------------------------------------------

// 16 kHz → 8 kHz. `n_in` samples in (must be even), n_in/2 out. Two-tap
// average — a cheap anti-alias that tames the top octave before decimation.
inline void downsample_2x(const int16_t* in, std::size_t n_in, int16_t* out)
{
    for (std::size_t k = 0; k < n_in / 2; ++k)
        out[k] = static_cast<int16_t>((static_cast<int32_t>(in[2 * k]) + in[2 * k + 1]) / 2);
}

// 8 kHz → 16 kHz. `n_in` samples in, 2*n_in out. Linear interpolation.
inline void upsample_2x(const int16_t* in, std::size_t n_in, int16_t* out)
{
    for (std::size_t k = 0; k < n_in; ++k) {
        const int32_t cur = in[k];
        const int32_t nxt = (k + 1 < n_in) ? in[k + 1] : cur;
        out[2 * k]     = static_cast<int16_t>(cur);
        out[2 * k + 1] = static_cast<int16_t>((cur + nxt) / 2);
    }
}

// ---- frame helpers (16 kHz PCM ↔ 8 kHz µ-law on the wire) -----------------

// Encode a 16 kHz mono frame to µ-law: downsample to 8 kHz then compand.
// `n16` must be even; writes n16/2 µ-law bytes; returns the byte count.
inline std::size_t g711_encode_16k(const int16_t* pcm16, std::size_t n16, uint8_t* ulaw_out)
{
    const std::size_t n8 = n16 / 2;
    for (std::size_t k = 0; k < n8; ++k) {
        const int16_t s = static_cast<int16_t>(
            (static_cast<int32_t>(pcm16[2 * k]) + pcm16[2 * k + 1]) / 2);
        ulaw_out[k] = ulaw_encode(s);
    }
    return n8;
}

// Decode µ-law bytes to a 16 kHz mono frame: expand then upsample.
// Writes 2*n_bytes samples into pcm16_out; returns that sample count.
inline std::size_t g711_decode_to_16k(const uint8_t* ulaw, std::size_t n_bytes, int16_t* pcm16_out)
{
    for (std::size_t k = 0; k < n_bytes; ++k) {
        const int32_t cur = ulaw_decode(ulaw[k]);
        const int32_t nxt = (k + 1 < n_bytes) ? ulaw_decode(ulaw[k + 1]) : cur;
        pcm16_out[2 * k]     = static_cast<int16_t>(cur);
        pcm16_out[2 * k + 1] = static_cast<int16_t>((cur + nxt) / 2);
    }
    return n_bytes * 2;
}

} // namespace domain
