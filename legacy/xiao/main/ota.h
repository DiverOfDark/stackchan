#pragma once

#include "esp_err.h"

// HTTPS-based OTA update via esp_https_ota. Caller provides the URL of the
// new firmware.bin (typically served from the same host as the pipecat
// backend, e.g. https://pipecat.example.com/firmware/firmware.bin).
//
// Blocks until either the new image is written to the inactive OTA slot
// (esp_restart on success) or the download/verify fails. On failure no
// changes are made to the running slot.

esp_err_t ota_check_and_update(const char *url);
