#pragma once

#include <stdint.h>

#include "esp_err.h"

// I2C control interface to the XMOS XVF3800 voice processor on the Seeed
// ReSpeaker carrier. The XVF3800 owns the WS2812 LED ring (it drives the
// data line internally); we ask it to render an effect/colour/brightness
// via control-protocol writes at I2C address 0x2C.
//
// Wire format for every control write (per Seeed example,
// wiki.seeedstudio.com/respeaker_xvf3800_xiao_rgb/):
//
//   [I2C addr 0x2C][resource_id][command_id][payload_len N][payload bytes...]
//
// All LED commands use resource_id = 20 (GPO servicer).

#define XVF3800_I2C_ADDR       0x2C
#define XVF3800_RESID_GPO      20

#define XVF3800_CMD_LED_EFFECT      12
#define XVF3800_CMD_LED_BRIGHTNESS  13
#define XVF3800_CMD_LED_SPEED       15
#define XVF3800_CMD_LED_COLOR       16

// Default XIAO ESP32-S3 I2C pin assignment. Override at build time if your
// carrier rewires them.
#ifndef XVF3800_I2C_SDA_GPIO
#define XVF3800_I2C_SDA_GPIO   5
#endif
#ifndef XVF3800_I2C_SCL_GPIO
#define XVF3800_I2C_SCL_GPIO   6
#endif

esp_err_t xvf3800_init(void);

// Generic control write — write_bytes_n bytes of payload after the
// 3-byte header. Returns ESP_OK on ack, propagates the IDF error otherwise.
esp_err_t xvf3800_write(uint8_t resid, uint8_t cmd,
                        const uint8_t *payload, uint8_t n);

// LED ring convenience wrappers. Pass-through to xvf3800_write with the
// right resource + command IDs. Safe to call from any task context (each
// call is a single short I2C transaction).
esp_err_t xvf3800_set_led_effect    (uint8_t effect);     // 0=off, 1=solid, ... (Seeed firmware-specific)
esp_err_t xvf3800_set_led_brightness(uint8_t brightness); // 0..255
esp_err_t xvf3800_set_led_speed     (uint8_t speed);
esp_err_t xvf3800_set_led_color     (uint8_t r, uint8_t g, uint8_t b);
