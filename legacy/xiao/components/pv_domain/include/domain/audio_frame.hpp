#pragma once

// Audio shape constants + frame types. Pure compile-time declarations —
// no .cpp file. Used by everyone who touches PCM (HAL, Transport, App).
//
// Sample rate is fixed at 16 kHz mono for both the mic uplink and the
// decoder output. Choice is set by the backend: faster-whisper STT
// expects 16 kHz, Piper synthesises 22 kHz but pipecat resamples to
// 48 kHz Opus on the wire, which we decode and resample down to 16 kHz
// for the I2S DAC.

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
