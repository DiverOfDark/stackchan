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

// Max bytes we ever pass to Opus encode/decode for one frame. Sized for
// the worst-case 48 kHz / 120 ms FEC payload so a misbehaving sender
// can't overflow even if backend ever switches modes.
inline constexpr std::size_t kOpusMaxPacketBytes = 1500;
inline constexpr std::size_t kOpusMaxDecodedSamples = 5760;  // 48 kHz × 120 ms

} // namespace domain
