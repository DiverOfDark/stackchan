#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Two-mode Wi-Fi bring-up:
//   1. If NVS has saved SSID + PSK + backend URL, connect to the saved
//      station and expose the backend URL via wifi_provision_backend_url().
//   2. Otherwise, start a SoftAP named "pipecat-voice-XXXX" (XXXX = last 2
//      bytes of the STA MAC, hex) and serve a captive portal at
//      http://192.168.4.1/. The form POST saves to NVS and reboots.

#define WIFI_PROVISION_AP_PREFIX "pipecat-voice-"
#define WIFI_PROVISION_AP_PASSWORD ""          // open AP for setup
#define WIFI_PROVISION_HTTP_PORT 80

typedef enum {
    WIFI_PROV_STATE_BOOT,
    WIFI_PROV_STATE_CONNECTING,
    WIFI_PROV_STATE_CONNECTED,
    WIFI_PROV_STATE_PROVISIONING,
    WIFI_PROV_STATE_FAILED,
} wifi_provision_state_t;

// Init NVS + Wi-Fi stack and decide automatically between STA-connect and
// SoftAP-provisioning. Returns immediately; state transitions happen on the
// system event loop. Call once from app_main.
esp_err_t wifi_provision_start(void);

wifi_provision_state_t wifi_provision_state(void);

// Returns NULL until WIFI_PROV_STATE_CONNECTED is reached. The pointer is
// stable for the rest of the boot.
const char *wifi_provision_backend_url(void);
