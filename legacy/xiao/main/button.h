#pragma once

#include <stdbool.h>

#include "esp_err.h"

// User button + mute-switch handling. The XIAO ESP32-S3 carries a BOOT
// button on GPIO 0; the XVF3800 ReSpeaker carrier exposes a mute switch on
// a separate GPIO (override via `BUTTON_MUTE_GPIO`).
//
// - Short press (< 1 s): toggle mute. Mic uplink frames are gated by
//   button_is_muted(); webrtc_session honours this on its capture path.
// - Long press (≥ 5 s): wipe NVS Wi-Fi creds and reboot into provisioning
//   mode. Useful when moving the device to a new network.

#ifndef BUTTON_MUTE_GPIO
#define BUTTON_MUTE_GPIO  0   // BOOT button on the XIAO ESP32-S3
#endif

esp_err_t button_init(void);
bool      button_is_muted(void);
