#pragma once

// G.722 codec — 16 kHz wideband ADPCM, 64 kbit/s. Pure, host-testable.
//
// Why G.722 (replacing the earlier G.711 µ-law path): G.711 is 8 kHz
// narrowband — telephone quality, ~3.4 kHz of audio bandwidth, which is why
// the speaker sounded muffled. G.722 doubles that to ~7 kHz wideband ("FM
// radio" vs "phone call") while staying cheap: it's plain ADPCM + a 24-tap
// QMF, no large scratch buffers, so unlike Opus it encodes *and* decodes in
// real time on this ESP32-S3 with room to spare. Bonus: the 16 kHz uplink it
// produces is exactly what Whisper wants, so STT improves too.
//
// On the wire G.722 is, like G.711, 64 kbit/s: a 20 ms frame is 320 input
// samples @ 16 kHz -> 160 octets, and its RTP clock is (by RFC 3551 quirk)
// declared as 8000 Hz, so libpeer's existing PCMU packetiser/timestamping is
// reused unchanged — only the payload type (9) and the codec math differ.
//
// Unlike G.711, both encode and decode are STATEFUL (adaptive predictors +
// QMF delay lines). Hold one G722Codec for the uplink encoder and a separate
// one for the downlink decoder, and reset (g722_init) them whenever the peer
// connection is rebuilt so a fresh stream starts from the standard state.
//
// This is a faithful port of Steve Underwood's SpanDSP g722.c (the de-facto
// ITU-T G.722 reference), trimmed to the single mode we use: 64 kbit/s,
// 16 kHz, unpacked, normal (non-test) operation. Original is LGPL-2.1; see
// src/g722.cpp for the full attribution. Keeping it ITU-compliant is what lets
// it interoperate with aiortc's G.722 on the backend.

#include <cstddef>
#include <cstdint>

namespace domain {

// Per-band adaptive predictor state (low band = band[0], high band = band[1]).
struct G722Band {
    int16_t nb   = 0;
    int16_t det  = 0;
    int16_t s    = 0;
    int16_t sz   = 0;
    int16_t r    = 0;
    int16_t p[2] = {0, 0};
    int16_t a[2] = {0, 0};
    int16_t b[6] = {0, 0, 0, 0, 0, 0};
    int16_t d[7] = {0, 0, 0, 0, 0, 0, 0};
};

// One codec instance. Use one for encoding (uplink), a separate one for
// decoding (downlink) — they must not share state.
struct G722Codec {
    int16_t   x[12]   = {0};   // QMF history (first of each sample pair)
    int16_t   y[12]   = {0};   // QMF history (second of each sample pair)
    int       ptr     = 0;     // circular index into x/y
    G722Band  band[2];
};

// Reset to the standard G.722 initial state. Call before first use and on
// every reconnect.
void g722_init(G722Codec& c);

// Encode `len` 16 kHz mono samples (len must be even) into `out`; writes
// len/2 octets and returns that byte count.
std::size_t g722_encode(G722Codec& c, const int16_t* amp, std::size_t len, uint8_t* out);

// Decode `n_bytes` G.722 octets into 16 kHz mono samples; writes 2*n_bytes
// samples into `amp` and returns that sample count.
std::size_t g722_decode(G722Codec& c, const uint8_t* in, std::size_t n_bytes, int16_t* amp);

} // namespace domain
