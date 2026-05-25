#include "transport/opus_codec.hpp"

#include <utility>

#include "esp_log.h"

namespace {
constexpr const char* kTag = "opus_codec";
} // namespace

namespace transport {

// ---------- Encoder -------------------------------------------------------

std::optional<OpusEncoder> OpusEncoder::create(int sample_rate)
{
    int err = 0;
    ::OpusEncoder* enc = opus_encoder_create(
        sample_rate, /*channels=*/1, OPUS_APPLICATION_VOIP, &err);
    if (!enc || err != OPUS_OK) {
        ESP_LOGE(kTag, "opus_encoder_create failed: %d", err);
        if (enc) opus_encoder_destroy(enc);
        return std::nullopt;
    }
    opus_encoder_ctl(enc, OPUS_SET_BITRATE(kBitrateBps));
    opus_encoder_ctl(enc, OPUS_SET_COMPLEXITY(kComplexity));
    opus_encoder_ctl(enc, OPUS_SET_SIGNAL(kSignalKind));
    opus_encoder_ctl(enc, OPUS_SET_INBAND_FEC(1));
    opus_encoder_ctl(enc, OPUS_SET_PACKET_LOSS_PERC(kPacketLossPct));
    // DTX is intentionally NOT enabled — see note in capture_pipeline
    // about the silk_burg_modified_FLP watchdog.
    return OpusEncoder{enc};
}

OpusEncoder::OpusEncoder(OpusEncoder&& other) noexcept : enc_(other.enc_)
{
    other.enc_ = nullptr;
}

OpusEncoder& OpusEncoder::operator=(OpusEncoder&& other) noexcept
{
    if (this != &other) {
        if (enc_) opus_encoder_destroy(enc_);
        enc_ = other.enc_;
        other.enc_ = nullptr;
    }
    return *this;
}

OpusEncoder::~OpusEncoder()
{
    if (enc_) opus_encoder_destroy(enc_);
}

int OpusEncoder::encode(const int16_t* pcm, std::size_t n_samples,
                        uint8_t* out, std::size_t out_cap)
{
    if (!enc_ || !pcm || !out) return OPUS_BAD_ARG;
    return opus_encode(enc_, pcm, static_cast<int>(n_samples),
                       out, static_cast<int>(out_cap));
}

// ---------- Decoder -------------------------------------------------------

std::optional<OpusDecoder> OpusDecoder::create(int sample_rate)
{
    int err = 0;
    ::OpusDecoder* dec = opus_decoder_create(sample_rate, /*channels=*/1, &err);
    if (!dec || err != OPUS_OK) {
        ESP_LOGE(kTag, "opus_decoder_create failed: %d", err);
        if (dec) opus_decoder_destroy(dec);
        return std::nullopt;
    }
    return OpusDecoder{dec};
}

OpusDecoder::OpusDecoder(OpusDecoder&& other) noexcept : dec_(other.dec_)
{
    other.dec_ = nullptr;
}

OpusDecoder& OpusDecoder::operator=(OpusDecoder&& other) noexcept
{
    if (this != &other) {
        if (dec_) opus_decoder_destroy(dec_);
        dec_ = other.dec_;
        other.dec_ = nullptr;
    }
    return *this;
}

OpusDecoder::~OpusDecoder()
{
    if (dec_) opus_decoder_destroy(dec_);
}

int OpusDecoder::decode(const uint8_t* pkt, std::size_t pkt_size,
                        int16_t* out, std::size_t out_cap)
{
    if (!dec_ || !pkt || !out) return OPUS_BAD_ARG;
    return opus_decode(dec_, pkt, static_cast<int>(pkt_size),
                       out, static_cast<int>(out_cap), /*decode_fec=*/0);
}

} // namespace transport
