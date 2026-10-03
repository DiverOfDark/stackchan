#include "femto_vision.h"

#include "esp_camera.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "human_face_detect.hpp"

static const char *TAG = "femto_vision";
static HumanFaceDetect *s_detect = nullptr;

extern "C" esp_err_t femto_vision_init(int i2c_port)
{
    camera_config_t c = {};
    c.pin_pwdn = -1;
    c.pin_reset = -1;
    c.pin_xclk = -1;  // GC0308 runs from the board's own 20 MHz oscillator
    c.pin_sccb_sda = -1;  // share the I2C port the firmware already owns
    c.pin_sccb_scl = -1;
    c.sccb_i2c_port = i2c_port;
    c.pin_d7 = 47;
    c.pin_d6 = 48;
    c.pin_d5 = 16;
    c.pin_d4 = 15;
    c.pin_d3 = 42;
    c.pin_d2 = 41;
    c.pin_d1 = 40;
    c.pin_d0 = 39;
    c.pin_vsync = 46;
    c.pin_href = 38;
    c.pin_pclk = 45;
    c.xclk_freq_hz = 20000000;
    c.ledc_timer = LEDC_TIMER_0;
    c.ledc_channel = LEDC_CHANNEL_0;
    c.pixel_format = PIXFORMAT_RGB565;
    c.frame_size = FRAMESIZE_QQVGA;  // 160x120: the detector's first stage runs at this size anyway
    c.jpeg_quality = 0;
    c.fb_count = 1;
    c.fb_location = CAMERA_FB_IN_PSRAM;
    c.grab_mode = CAMERA_GRAB_WHEN_EMPTY;  // capture on demand: keep PSRAM free for rendering

    esp_err_t err = esp_camera_init(&c);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera init: %s", esp_err_to_name(err));
        return err;
    }
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        ESP_LOGI(TAG, "sensor PID 0x%04x", s->id.PID);
        s->set_hmirror(s, 1);  // so "left" in the frame is the robot's left
    }
    s_detect = new HumanFaceDetect();
    return ESP_OK;
}

extern "C" int femto_vision_step(femto_face_t *out, int max, uint32_t *detect_ms)
{
    if (!s_detect) {
        return -1;
    }
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        return -1;
    }
    dl::image::img_t img = {};
    img.data = fb->buf;
    img.width = (uint16_t)fb->width;
    img.height = (uint16_t)fb->height;
    img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565BE;

    int64_t t0 = esp_timer_get_time();
    std::list<dl::detect::result_t> &res = s_detect->run(img);
    if (detect_ms) {
        *detect_ms = (uint32_t)((esp_timer_get_time() - t0) / 1000);
    }
    int n = 0;
    for (auto &r : res) {
        if (n >= max) {
            break;
        }
        // Report in 320x240 space whatever the capture size.
        const int sx = 320 / fb->width, sy = 240 / fb->height;
        out[n].x1 = r.box[0] * sx;
        out[n].y1 = r.box[1] * sy;
        out[n].x2 = r.box[2] * sx;
        out[n].y2 = r.box[3] * sy;
        out[n].score = r.score;
        n++;
    }
    esp_camera_fb_return(fb);
    return n;
}

extern "C" void femto_vision_stop(void)
{
    sensor_t *s = esp_camera_sensor_get();
    if (s && s->set_reg) {
        // GC0308: page 0, reg 0x25 (output enable) = 0 tri-states PCLK/data.
        s->set_reg(s, 0xFE, 0xFF, 0x00);
        s->set_reg(s, 0x25, 0xFF, 0x00);
    }
    esp_camera_deinit();
}
