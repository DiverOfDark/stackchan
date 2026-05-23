#pragma once

#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

// I2S audio I/O for the ReSpeaker XVF3800 + XIAO ESP32-S3 carrier.
// Pin assignment is fixed by the carrier board: BCK=8, WS=7, TX=44, RX=43.
// XVF3800 acts as I2S master (provides BCK + WS); ESP32-S3 is the slave.
//
// Frame layout on the wire (XVF3800 default I2S firmware):
//   16 kHz, stereo, 32-bit per sample (Philips standard).
//   - RX from XVF3800: post-DSP audio. Exact channel meaning (mono mix /
//     left-beam / right-beam / mic-ref) depends on XVF3800 runtime config;
//     defaults to channel 0 = processed mono.
//   - TX to XVF3800: speaker playback. XVF3800 uses one channel as AEC
//     reference internally (see notes in xvf3800.c / M5).

#define AUDIO_IO_SAMPLE_RATE      16000
#define AUDIO_IO_CHANNELS         2
#define AUDIO_IO_BITS_PER_SAMPLE  32

// Initialize the I2S peripheral. Idempotent.
esp_err_t audio_io_init(void);

// Blocking read of `frames` stereo frames (one frame = 2x int32_t) into `dst`.
// Returns ESP_OK only if all frames were read. `dst` must hold
// frames * AUDIO_IO_CHANNELS int32_t.
esp_err_t audio_io_read(int32_t *dst, size_t frames);

// Blocking write of `frames` stereo frames.
esp_err_t audio_io_write(const int32_t *src, size_t frames);

// Launch a background task that continuously reads from the mic and writes
// the same buffer back to the speaker — the M1 sanity-check loopback.
// Idempotent. Pass NULL to use a sensible default frame count per iteration.
esp_err_t audio_io_start_loopback(void);
