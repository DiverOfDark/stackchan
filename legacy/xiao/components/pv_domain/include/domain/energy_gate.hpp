#pragma once

// Cheap integer audio-energy primitives. Used by the capture pipeline to
// drive the TALKING LED and the conversation keep-alive timer. Pure
// functions; same source compiles on host for unit testing.

#include <cstddef>
#include <cstdint>

namespace domain {

// RMS of an int16 mono frame. Returns 0 for empty input. Uses an 8-
// iteration Newton's method integer sqrt at the end — adequate for
// our LED-gate purposes (we don't need full precision).
uint32_t rms_i16(const int16_t* samples, std::size_t n);

// Peak magnitude of an int16 frame. Returns 0 for empty input. Caller
// uses this for the inbound-TTS "speaking?" decision, because peak is
// what differentiates a real TTS frame (peak >= 1000) from aiortc
// comfort-noise (peak ~0).
int32_t peak_abs_i16(const int16_t* samples, std::size_t n);

} // namespace domain
