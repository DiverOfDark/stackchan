#include "xvf3800.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"

static const char *TAG = "xvf3800";

#define I2C_CLOCK_HZ          400000        // XVF3800 datasheet allows up to 400 kHz
#define I2C_TIMEOUT_MS        100
#define MAX_PAYLOAD_BYTES     32            // generous; LED ops use ≤ 4

static i2c_master_bus_handle_t s_bus = NULL;
static i2c_master_dev_handle_t s_dev = NULL;

esp_err_t xvf3800_init(void)
{
    if (s_dev) return ESP_OK;

    i2c_master_bus_config_t bus_cfg = {
        .clk_source                   = I2C_CLK_SRC_DEFAULT,
        .i2c_port                     = I2C_NUM_0,
        .scl_io_num                   = XVF3800_I2C_SCL_GPIO,
        .sda_io_num                   = XVF3800_I2C_SDA_GPIO,
        .glitch_ignore_cnt            = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return err;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address  = XVF3800_I2C_ADDR,
        .scl_speed_hz    = I2C_CLOCK_HZ,
    };
    err = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c add device 0x%02x failed: %s",
                 XVF3800_I2C_ADDR, esp_err_to_name(err));
        return err;
    }

    // Quick probe — ack at 0x2C confirms the bus is wired and the chip is
    // alive. Non-fatal on failure (logs a warning); the user might be
    // testing with the carrier disconnected.
    if (i2c_master_probe(s_bus, XVF3800_I2C_ADDR, I2C_TIMEOUT_MS) != ESP_OK) {
        ESP_LOGW(TAG, "no ack at 0x%02x — XVF3800 not responding "
                      "(check power, I2C pullups, SDA=%d SCL=%d)",
                 XVF3800_I2C_ADDR, XVF3800_I2C_SDA_GPIO, XVF3800_I2C_SCL_GPIO);
    } else {
        ESP_LOGI(TAG, "XVF3800 ack at 0x%02x (SDA=%d SCL=%d, %u Hz)",
                 XVF3800_I2C_ADDR, XVF3800_I2C_SDA_GPIO, XVF3800_I2C_SCL_GPIO,
                 (unsigned)I2C_CLOCK_HZ);
    }
    return ESP_OK;
}

esp_err_t xvf3800_write(uint8_t resid, uint8_t cmd,
                        const uint8_t *payload, uint8_t n)
{
    if (!s_dev) return ESP_ERR_INVALID_STATE;
    if (n > MAX_PAYLOAD_BYTES) return ESP_ERR_INVALID_SIZE;

    uint8_t buf[3 + MAX_PAYLOAD_BYTES];
    buf[0] = resid;
    buf[1] = cmd;
    buf[2] = n;
    if (n) memcpy(&buf[3], payload, n);

    return i2c_master_transmit(s_dev, buf, 3 + n, I2C_TIMEOUT_MS);
}

esp_err_t xvf3800_set_led_effect(uint8_t effect)
{
    return xvf3800_write(XVF3800_RESID_GPO, XVF3800_CMD_LED_EFFECT, &effect, 1);
}

esp_err_t xvf3800_set_led_brightness(uint8_t brightness)
{
    return xvf3800_write(XVF3800_RESID_GPO, XVF3800_CMD_LED_BRIGHTNESS, &brightness, 1);
}

esp_err_t xvf3800_set_led_speed(uint8_t speed)
{
    return xvf3800_write(XVF3800_RESID_GPO, XVF3800_CMD_LED_SPEED, &speed, 1);
}

esp_err_t xvf3800_set_led_color(uint8_t r, uint8_t g, uint8_t b)
{
    // Seeed example serialises color LSB-first into a 4-byte payload as
    // (B, G, R, 0x00) — see wiki.seeedstudio.com/respeaker_xvf3800_xiao_rgb/.
    uint8_t payload[4] = { b, g, r, 0x00 };
    return xvf3800_write(XVF3800_RESID_GPO, XVF3800_CMD_LED_COLOR, payload, 4);
}
