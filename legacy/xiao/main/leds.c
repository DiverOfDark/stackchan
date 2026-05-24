#include "leds.h"

#include "esp_log.h"

#include "xvf3800.h"

static const char *TAG = "leds";

// XVF3800 LED "effects" are firmware-defined integers — the Seeed example
// only uses 1 ("solid") and doesn't enumerate the rest. We pick values
// conservatively (1=solid for steady states, a higher-numbered animation
// for the moving states) and adjust empirically on hardware if the chosen
// effect ID renders something different than expected.
#define EFFECT_OFF      0
#define EFFECT_SOLID    1
#define EFFECT_PULSE    2     // soft breathing
#define EFFECT_SPIN     3     // rotating dot/segment

// Colour palette in 24-bit RGB. Tuned for daylight visibility on the
// Seeed carrier's diffused ring.
#define RGB_BLUE        0x00, 0x80, 0xFF
#define RGB_GREEN       0x00, 0xC8, 0x40
#define RGB_RED         0xFF, 0x20, 0x10
#define RGB_AMBER       0xFF, 0x88, 0x00

static led_state_t s_prev = LED_STATE_OFF;

static const char *state_name(led_state_t s)
{
    switch (s) {
    case LED_STATE_OFF:           return "off";
    case LED_STATE_PROVISIONING:  return "provisioning";
    case LED_STATE_CONNECTING:    return "connecting";
    case LED_STATE_LISTENING:     return "listening";
    case LED_STATE_SPEAKING:      return "speaking";
    case LED_STATE_MUTED:         return "muted";
    case LED_STATE_ERROR:         return "error";
    }
    return "?";
}

esp_err_t leds_init(void)
{
    esp_err_t err = xvf3800_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "xvf3800_init failed (%s) — LEDs will be no-ops",
                 esp_err_to_name(err));
        return ESP_OK;     // non-fatal; firmware still works without LEDs
    }
    // Establish a sane baseline so a brand-new device shows *something*
    // immediately. The provisioning state is the most common boot path
    // (no NVS credentials yet); main.c overrides this with leds_set()
    // once it has actually decided what state we're in.
    xvf3800_set_led_brightness(0x60);
    xvf3800_set_led_speed(0x40);
    xvf3800_set_led_effect(EFFECT_OFF);
    return ESP_OK;
}

void leds_set(led_state_t s)
{
    if (s == s_prev) return;
    s_prev = s;
    ESP_LOGI(TAG, "→ %s", state_name(s));

    switch (s) {
    case LED_STATE_OFF:
        xvf3800_set_led_effect(EFFECT_OFF);
        break;
    case LED_STATE_PROVISIONING:
        xvf3800_set_led_color(RGB_BLUE);
        xvf3800_set_led_speed(0x20);            // slow breath
        xvf3800_set_led_effect(EFFECT_PULSE);
        break;
    case LED_STATE_CONNECTING:
        xvf3800_set_led_color(RGB_BLUE);
        xvf3800_set_led_speed(0x80);            // brisk spin
        xvf3800_set_led_effect(EFFECT_SPIN);
        break;
    case LED_STATE_LISTENING:
        xvf3800_set_led_color(RGB_GREEN);
        xvf3800_set_led_effect(EFFECT_SOLID);
        break;
    case LED_STATE_SPEAKING:
        xvf3800_set_led_color(RGB_GREEN);
        xvf3800_set_led_speed(0xA0);            // fast spin while TTS plays
        xvf3800_set_led_effect(EFFECT_SPIN);
        break;
    case LED_STATE_MUTED:
        xvf3800_set_led_color(RGB_RED);
        xvf3800_set_led_effect(EFFECT_SOLID);
        break;
    case LED_STATE_ERROR:
        xvf3800_set_led_color(RGB_RED);
        xvf3800_set_led_speed(0xC0);            // urgent
        xvf3800_set_led_effect(EFFECT_PULSE);
        break;
    }
}
