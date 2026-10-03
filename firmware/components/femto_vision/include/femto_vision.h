// C shim over esp32-camera (GC0308) and ESP-DL human_face_detect, so the
// Rust firmware only deals with plain structs (PRD §8.2, option C).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** One detected face, in 320x240 coordinates (scaled from the capture). */
typedef struct {
    int16_t x1, y1, x2, y2;
    float score;
} femto_face_t;

/** Camera on the StackChan pins, QVGA RGB565, SCCB over the already
 *  installed legacy I2C driver on `i2c_port`. Loads the face model. */
esp_err_t femto_vision_init(int i2c_port);

/** Grab the latest frame and detect faces. Returns the number written to
 *  `out` (≤ max), or -1 when no frame was available. `detect_ms` gets the
 *  inference time. */
int femto_vision_step(femto_face_t *out, int max, uint32_t *detect_ms);

/** Copy the most recent frame (raw RGB565 as captured, big-endian) into
 *  `buf`. Returns bytes written (0 if none yet); `w`/`h` get its size. */
size_t femto_vision_last_frame(uint8_t *buf, size_t cap, uint16_t *w, uint16_t *h);

/** Stop streaming and put the sensor in standby (call before restarting:
 *  the camera's PCLK sits on strapping pin GPIO45). */
void femto_vision_stop(void);

#ifdef __cplusplus
}
#endif
