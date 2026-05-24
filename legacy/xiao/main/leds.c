#include "leds.h"

#include "esp_log.h"

static const char *TAG = "leds";

// XVF3800 I2C control commands for the LED ring (per XMOS XVF3800
// Programming Guide v2.0.0, Appendix AA — to be wired into xvf3800.c).
// The chip drives the 12 LEDs itself; we send a single I2C transaction per
// state change.
//
// CONTROL_LED_RING_MODE = SOLID | PULSE | SPIN
// CONTROL_LED_RING_COLOUR = RGB triple
//
// For now we just log transitions so the app code can be wired and the
// hardware semantics dropped in later without API churn.

static const char *state_name(led_state_t s)
{
    switch (s) {
    case LED_STATE_OFF:           return "off";
    case LED_STATE_PROVISIONING:  return "provisioning (slow pulse, blue)";
    case LED_STATE_CONNECTING:    return "connecting (spin, blue)";
    case LED_STATE_LISTENING:     return "listening (solid, green)";
    case LED_STATE_SPEAKING:      return "speaking (spin to DoA, green)";
    case LED_STATE_MUTED:         return "muted (solid, red)";
    case LED_STATE_ERROR:         return "error (solid, red, fast blink)";
    }
    return "?";
}

esp_err_t leds_init(void)
{
    ESP_LOGI(TAG, "led ring driver scaffolded; XVF3800 I2C cmds TBD");
    return ESP_OK;
}

void leds_set(led_state_t s)
{
    static led_state_t prev = LED_STATE_OFF;
    if (s == prev) return;
    prev = s;
    ESP_LOGI(TAG, "→ %s", state_name(s));
    // TODO(M7-followup): xvf3800_send_cmd(CONTROL_LED_RING_MODE, ...);
    //                    xvf3800_send_cmd(CONTROL_LED_RING_COLOUR, r, g, b);
}
