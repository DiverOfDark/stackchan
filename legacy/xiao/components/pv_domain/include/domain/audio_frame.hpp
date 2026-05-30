#pragma once

// Audio shape constants + frame types. Pure compile-time declarations —
// no .cpp file. Used by everyone who touches PCM (HAL, Transport, App).
//
// The mic / I2S path runs at 16 kHz mono. The WebRTC wire codec is G.711
// µ-law @ 8 kHz (PCMU) — Opus's large SILK scratch can't fit in this board's
// internal RAM and ran slower than real time (see domain/g711.hpp). We
// down/upsample 16↔8 kHz at the codec boundary; the local wake word still
// uses the full 16 kHz mic path.

#include <array>
#include <cstddef>
#include <cstdint>

namespace domain {

inline constexpr int   kSampleRateHz   = 16'000;
inline constexpr int   kFrameDurationMs = 20;
inline constexpr std::size_t kFramesPerPacket = kSampleRateHz * kFrameDurationMs / 1000;
inline constexpr int   kChannels       = 1;

// One 20 ms frame of mono int16 PCM at 16 kHz. Stack-allocated; copies
// are cheap (640 bytes). Pass by reference when you want to hand off
// ownership, by const-ref when you only read.
using Frame16 = std::array<int16_t, kFramesPerPacket>;

// Decode scratch for one inbound audio packet, in 16 kHz samples. A 20 ms
// PCMU frame is 160 bytes → 320 samples; this is sized generously so an
// oversized inbound packet can't overflow the playback decode buffer.
inline constexpr std::size_t kMaxDecodedSamples = 2048;

} // namespace domain
