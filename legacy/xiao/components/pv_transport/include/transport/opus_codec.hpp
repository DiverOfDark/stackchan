#pragma once

// RAII wrappers around micro-opus encoder + decoder handles. Mono
// 16 kHz at OPUS_BITRATE_BPS bits/sec; OPUS_COMPLEXITY = 0 because
// micro-opus's float SILK Burg LPC analysis at higher complexity
// trips the ESP32-S3 task watchdog (chased that one for a session).

#include <cstddef>
#include <cstdint>
#include <optional>

#include "domain/audio_frame.hpp"
#include "opus.h"

namespace transport {

class OpusEncoder {
public:
    static constexpr int kBitrateBps   = 24'000;
    static constexpr int kComplexity   = 0;
    static constexpr int kSignalKind   = OPUS_SIGNAL_VOICE;
    static constexpr int kPacketLossPct = 10;

    static std::optional<OpusEncoder> create(int sample_rate = domain::kSampleRateHz);

    OpusEncoder(const OpusEncoder&)            = delete;
    OpusEncoder& operator=(const OpusEncoder&) = delete;
    OpusEncoder(OpusEncoder&& other) noexcept;
    OpusEncoder& operator=(OpusEncoder&& other) noexcept;
    ~OpusEncoder();

    // Encode one frame (kFramesPerPacket samples) of mono int16 PCM
    // into `out` (must be ≥ kOpusMaxPacketBytes). Returns the encoded
    // size on success, negative Opus error code on failure.
    int encode(const int16_t* pcm, std::size_t n_samples,
               uint8_t* out, std::size_t out_cap);

private:
    explicit OpusEncoder(::OpusEncoder* enc) noexcept : enc_(enc) {}
    ::OpusEncoder* enc_ = nullptr;
};

class OpusDecoder {
public:
    static std::optional<OpusDecoder> create(int sample_rate = domain::kSampleRateHz);

    OpusDecoder(const OpusDecoder&)            = delete;
    OpusDecoder& operator=(const OpusDecoder&) = delete;
    OpusDecoder(OpusDecoder&& other) noexcept;
    OpusDecoder& operator=(OpusDecoder&& other) noexcept;
    ~OpusDecoder();

    // Decode one Opus packet into `out` (must be ≥ kOpusMaxDecodedSamples).
    // Returns sample count on success, negative on failure.
    int decode(const uint8_t* pkt, std::size_t pkt_size,
               int16_t* out, std::size_t out_cap);

private:
    explicit OpusDecoder(::OpusDecoder* dec) noexcept : dec_(dec) {}
    ::OpusDecoder* dec_ = nullptr;
};

} // namespace transport
