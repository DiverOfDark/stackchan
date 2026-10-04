// G.722 codec implementation — see domain/g722.hpp for rationale.
//
// Faithful port of SpanDSP's g722.c by Steve Underwood <steveu@coppice.org>,
// Copyright (C) 2005 Steve Underwood, licensed LGPL-2.1. Trimmed to the single
// operating mode this device uses: 64 kbit/s, 16 kHz input/output, unpacked
// octets, normal (non ITU-test) operation. The quantiser tables and block4
// adaptation are the ITU-T G.722 reference data, reproduced verbatim so the
// bitstream interoperates with any compliant decoder/encoder (e.g. aiortc's
// G.722 on the backend).

#include "domain/g722.hpp"

#include <cstdlib>

namespace domain {
namespace {

// ---- saturation helpers (SpanDSP saturated.h) ---------------------------
inline int16_t saturate16(int32_t amp) {
    if (amp > 32767)  return 32767;
    if (amp < -32768) return -32768;
    return static_cast<int16_t>(amp);
}
inline int16_t saturate15(int32_t amp) {
    if (amp > 16383)  return 16383;
    if (amp < -16384) return -16384;
    return static_cast<int16_t>(amp);
}
inline int16_t sat_add16(int16_t a, int16_t b) { return saturate16(static_cast<int32_t>(a) + b); }
inline int16_t sat_sub16(int16_t a, int16_t b) { return saturate16(static_cast<int32_t>(a) - b); }

// Circular dot product over a 12-tap history starting at `pos` (SpanDSP
// vec_circular_dot_prodi16, specialised to n == 12).
inline int32_t circ_dot12(const int16_t* x, const int16_t* coeffs, int pos) {
    int32_t z = 0;
    int i = 0;
    for (; pos + i < 12; ++i) z += static_cast<int32_t>(x[pos + i]) * coeffs[i];
    for (int k = 0; k < pos; ++k, ++i) z += static_cast<int32_t>(x[k]) * coeffs[i];
    return z;
}

// ---- ITU-T G.722 reference tables ---------------------------------------
const int16_t qmf_coeffs_fwd[12] = {  3, -11,  12,  32, -210,  951, 3876, -805,  362, -156,  53, -11 };
const int16_t qmf_coeffs_rev[12] = { -11,  53, -156, 362, -805, 3876,  951, -210,   32,   12, -11,   3 };

const int16_t qm2[4]  = { -7408, -1616, 7408, 1616 };
const int16_t qm4[16] = {     0, -20456, -12896, -8968, -6288, -4240, -2584, -1200,
                          20456,  12896,   8968,  6288,  4240,  2584,  1200,     0 };
const int16_t qm6[64] = {
     -136,   -136,   -136,   -136, -24808, -21904, -19008, -16704,
   -14984, -13512, -12280, -11192, -10232,  -9360,  -8576,  -7856,
    -7192,  -6576,  -6000,  -5456,  -4944,  -4464,  -4008,  -3576,
    -3168,  -2776,  -2400,  -2032,  -1688,  -1360,  -1040,   -728,
    24808,  21904,  19008,  16704,  14984,  13512,  12280,  11192,
    10232,   9360,   8576,   7856,   7192,   6576,   6000,   5456,
     4944,   4464,   4008,   3576,   3168,   2776,   2400,   2032,
     1688,   1360,   1040,    728,    432,    136,   -432,   -136 };
const int16_t q6[32] = {
       0,   35,   72,  110,  150,  190,  233,  276,
     323,  370,  422,  473,  530,  587,  650,  714,
     786,  858,  940, 1023, 1121, 1219, 1339, 1458,
    1612, 1765, 1980, 2195, 2557, 2919,    0,    0 };
const int16_t ilb[32] = {
    2048, 2093, 2139, 2186, 2233, 2282, 2332, 2383,
    2435, 2489, 2543, 2599, 2656, 2714, 2774, 2834,
    2896, 2960, 3025, 3091, 3158, 3228, 3298, 3371,
    3444, 3520, 3597, 3676, 3756, 3838, 3922, 4008 };
const int16_t iln[32] = {
     0, 63, 62, 31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19,
    18, 17, 16, 15, 14, 13, 12, 11, 10,  9,  8,  7,  6,  5,  4,  0 };
const int16_t ilp[32] = {
     0, 61, 60, 59, 58, 57, 56, 55, 54, 53, 52, 51, 50, 49, 48, 47,
    46, 45, 44, 43, 42, 41, 40, 39, 38, 37, 36, 35, 34, 33, 32,  0 };
const int16_t ihn[3] = { 0, 1, 0 };
const int16_t ihp[3] = { 0, 3, 2 };
const int16_t wl[8]  = { -60, -30, 58, 172, 334, 538, 1198, 3042 };
const int16_t rl42[16] = { 0, 7, 6, 5, 4, 3, 2, 1, 7, 6, 5, 4, 3, 2, 1, 0 };
const int16_t wh[3]  = { 0, -214, 798 };
const int16_t rh2[4] = { 2, 1, 2, 1 };

// Adaptive predictor update, shared by encode and decode (SpanDSP block4).
void block4(G722Band* s, int16_t dx) {
    int16_t wd1, wd2, wd3, sp, r, p, ap[2];
    int32_t wd32, sz;

    r = sat_add16(s->s, dx);   // RECONS
    p = sat_add16(s->sz, dx);  // PARREC

    // UPPOL2
    wd1  = saturate16(static_cast<int32_t>(s->a[0]) << 2);
    wd32 = ((p ^ s->p[0]) & 0x8000) ? wd1 : -wd1;
    if (wd32 > 32767) wd32 = 32767;
    wd3 = static_cast<int16_t>((((p ^ s->p[1]) & 0x8000) ? -128 : 128)
                               + (wd32 >> 7)
                               + ((static_cast<int32_t>(s->a[1]) * 32512) >> 15));
    if (std::abs(wd3) > 12288) wd3 = (wd3 < 0) ? -12288 : 12288;
    ap[1] = wd3;

    // UPPOL1
    wd1 = ((p ^ s->p[0]) & 0x8000) ? -192 : 192;
    wd2 = static_cast<int16_t>((static_cast<int32_t>(s->a[0]) * 32640) >> 15);
    ap[0] = sat_add16(wd1, wd2);
    wd3 = sat_sub16(15360, ap[1]);
    if (std::abs(ap[0]) > wd3) ap[0] = (ap[0] < 0) ? -wd3 : wd3;

    // FILTEP
    wd1 = sat_add16(r, r);
    wd1 = static_cast<int16_t>((static_cast<int32_t>(ap[0]) * wd1) >> 15);
    wd2 = sat_add16(s->r, s->r);
    wd2 = static_cast<int16_t>((static_cast<int32_t>(ap[1]) * wd2) >> 15);
    sp  = sat_add16(wd1, wd2);
    s->r    = r;
    s->a[1] = ap[1];
    s->a[0] = ap[0];
    s->p[1] = s->p[0];
    s->p[0] = p;

    // UPZERO / DELAYA / FILTEZ
    wd1 = (dx == 0) ? 0 : 128;
    s->d[0] = dx;
    sz = 0;
    for (int i = 5; i >= 0; --i) {
        wd2 = ((s->d[i + 1] ^ dx) & 0x8000) ? -wd1 : wd1;
        wd3 = static_cast<int16_t>((static_cast<int32_t>(s->b[i]) * 32640) >> 15);
        s->b[i] = sat_add16(wd2, wd3);
        wd3 = sat_add16(s->d[i], s->d[i]);
        sz += (static_cast<int32_t>(s->b[i]) * wd3) >> 15;
        s->d[i + 1] = s->d[i];
    }
    s->sz = saturate16(sz);

    // PREDIC
    s->s = sat_add16(sp, s->sz);
}

} // namespace

void g722_init(G722Codec& c) {
    c = G722Codec{};
    c.band[0].det = 32;
    c.band[1].det = 8;
}

std::size_t g722_encode(G722Codec& s, const int16_t* amp, std::size_t len, uint8_t* out) {
    std::size_t g722_bytes = 0;
    int16_t xlow, xhigh = 0;
    int16_t dlow, dhigh;

    for (std::size_t j = 0; j + 1 < len; ) {
        // Transmit QMF: feed a sample pair, split into low/high subbands.
        s.x[s.ptr] = amp[j++];
        s.y[s.ptr] = amp[j++];
        if (++s.ptr >= 12) s.ptr = 0;
        const int32_t sumodd  = circ_dot12(s.x, qmf_coeffs_fwd, s.ptr);
        const int32_t sumeven = circ_dot12(s.y, qmf_coeffs_rev, s.ptr);
        xlow  = static_cast<int16_t>((sumeven + sumodd) >> 14);
        xhigh = static_cast<int16_t>((sumeven - sumodd) >> 14);

        // ---- low band, 6-bit ADPCM ----
        const int el = sat_sub16(xlow, s.band[0].s);   // SUBTRA
        int wd = (el >= 0) ? el : ~el;                  // QUANTL
        int i;
        for (i = 1; i < 30; ++i) {
            const int wd1 = (static_cast<int32_t>(q6[i]) * s.band[0].det) >> 12;
            if (wd < wd1) break;
        }
        const int ilow = (el < 0) ? iln[i] : ilp[i];
        const int ril  = ilow >> 2;                     // INVQAL
        dlow = static_cast<int16_t>((static_cast<int32_t>(s.band[0].det) * qm4[ril]) >> 15);
        const int il4 = rl42[ril];                      // LOGSCL
        wd = (static_cast<int32_t>(s.band[0].nb) * 127) >> 7;
        s.band[0].nb = static_cast<int16_t>(wd + wl[il4]);
        if (s.band[0].nb < 0) s.band[0].nb = 0; else if (s.band[0].nb > 18432) s.band[0].nb = 18432;
        {                                               // SCALEL
            const int wd1 = (s.band[0].nb >> 6) & 31;
            const int wd2 = 8 - (s.band[0].nb >> 11);
            const int wd3 = (wd2 < 0) ? (ilb[wd1] << -wd2) : (ilb[wd1] >> wd2);
            s.band[0].det = static_cast<int16_t>(wd3 << 2);
        }
        block4(&s.band[0], dlow);

        // ---- high band, 2-bit ADPCM ----
        const int eh = sat_sub16(xhigh, s.band[1].s);   // SUBTRA
        wd = (eh >= 0) ? eh : ~eh;                       // QUANTH
        const int wd1h = (564 * s.band[1].det) >> 12;
        const int mih   = (wd >= wd1h) ? 2 : 1;
        const int ihigh = (eh < 0) ? ihn[mih] : ihp[mih];
        dhigh = static_cast<int16_t>((static_cast<int32_t>(s.band[1].det) * qm2[ihigh]) >> 15);
        const int ih2 = rh2[ihigh];                     // LOGSCH
        wd = (static_cast<int32_t>(s.band[1].nb) * 127) >> 7;
        s.band[1].nb = static_cast<int16_t>(wd + wh[ih2]);
        if (s.band[1].nb < 0) s.band[1].nb = 0; else if (s.band[1].nb > 22528) s.band[1].nb = 22528;
        {                                               // SCALEH
            const int wd1 = (s.band[1].nb >> 6) & 31;
            const int wd2 = 10 - (s.band[1].nb >> 11);
            const int wd3 = (wd2 < 0) ? (ilb[wd1] << -wd2) : (ilb[wd1] >> wd2);
            s.band[1].det = static_cast<int16_t>(wd3 << 2);
        }
        block4(&s.band[1], dhigh);

        out[g722_bytes++] = static_cast<uint8_t>((ihigh << 6) | ilow);
    }
    return g722_bytes;
}

std::size_t g722_decode(G722Codec& s, const uint8_t* in, std::size_t n_bytes, int16_t* amp) {
    std::size_t outlen = 0;
    int rlow, rhigh = 0;
    int16_t dlow, dhigh;

    for (std::size_t j = 0; j < n_bytes; ) {
        const int code = in[j++];
        int wd1   = code & 0x3F;
        const int ihigh = (code >> 6) & 0x03;
        int wd2   = qm6[wd1];
        wd1 >>= 2;

        // Block 5L / 6L: reconstruct + limit low band.
        wd2  = (static_cast<int32_t>(s.band[0].det) * wd2) >> 15;
        rlow = saturate15(s.band[0].s + wd2);
        // Block 2L INVQAL
        dlow = static_cast<int16_t>((static_cast<int32_t>(s.band[0].det) * qm4[wd1]) >> 15);
        // Block 3L LOGSCL
        wd2 = rl42[wd1];
        int nb = (static_cast<int32_t>(s.band[0].nb) * 127) >> 7;
        nb += wl[wd2];
        if (nb < 0) nb = 0; else if (nb > 18432) nb = 18432;
        s.band[0].nb = static_cast<int16_t>(nb);
        {   // SCALEL
            const int a = (s.band[0].nb >> 6) & 31;
            const int b = 8 - (s.band[0].nb >> 11);
            const int c = (b < 0) ? (ilb[a] << -b) : (ilb[a] >> b);
            s.band[0].det = static_cast<int16_t>(c << 2);
        }
        block4(&s.band[0], dlow);

        // High band.
        dhigh = static_cast<int16_t>((static_cast<int32_t>(s.band[1].det) * qm2[ihigh]) >> 15);
        rhigh = saturate15(dhigh + s.band[1].s);
        wd2 = rh2[ihigh];
        int nbh = (static_cast<int32_t>(s.band[1].nb) * 127) >> 7;
        nbh += wh[wd2];
        if (nbh < 0) nbh = 0; else if (nbh > 22528) nbh = 22528;
        s.band[1].nb = static_cast<int16_t>(nbh);
        {   // SCALEH
            const int a = (s.band[1].nb >> 6) & 31;
            const int b = 10 - (s.band[1].nb >> 11);
            const int c = (b < 0) ? (ilb[a] << -b) : (ilb[a] >> b);
            s.band[1].det = static_cast<int16_t>(c << 2);
        }
        block4(&s.band[1], dhigh);

        // Receive QMF: recombine the two subbands into a 16 kHz sample pair.
        s.x[s.ptr] = static_cast<int16_t>(rlow + rhigh);
        s.y[s.ptr] = static_cast<int16_t>(rlow - rhigh);
        if (++s.ptr >= 12) s.ptr = 0;
        amp[outlen++] = saturate16(circ_dot12(s.y, qmf_coeffs_rev, s.ptr) >> 11);
        amp[outlen++] = saturate16(circ_dot12(s.x, qmf_coeffs_fwd, s.ptr) >> 11);
    }
    return outlen;
}

} // namespace domain
