#include "leds.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "xvf3800.h"

static const char *TAG = "leds";

// XVF3800 effect IDs live in xvf3800.h next to the I2C command constants.
// Local aliases keep the switch below readable without re-exporting the
// XVF3800 namespace through every caller.
#define EFFECT_OFF      XVF3800_EFFECT_OFF
#define EFFECT_BREATH   XVF3800_EFFECT_BREATH
#define EFFECT_SOLID    XVF3800_EFFECT_SOLID

// Colour palette in 24-bit RGB. Tuned for daylight visibility on the
// Seeed carrier's diffused ring.
#define RGB_BLUE        0x00, 0x80, 0xFF
#define RGB_GREEN       0x00, 0xC8, 0x40
#define RGB_RED         0xFF, 0x20, 0x10
#define RGB_AMBER       0xFF, 0x88, 0x00
#define RGB_WHITE       0xFF, 0xFF, 0xFF
#define RGB_PINK        0xFF, 0x30, 0x80     // distinct from RED (mute) at a glance
#define RGB_PURPLE      0x80, 0x20, 0xFF     // negotiating — no green channel so the diffuser can't read "greenish" on this hw
#define RGB_CYAN        0x00, 0xC0, 0xFF     // TALKING — distinct from LISTENING green
// RGB_AMBER is defined above for the old NEGOTIATING state; reused for THINKING.

// How long a WAKE_ACK flash holds before the regular state machine takes
// over. Long enough to be visible against the LISTENING state right after.
#define WAKE_ACK_HOLD_MS  1200

static led_state_t s_prev          = LED_STATE_OFF;
static int64_t     s_hold_until_us = 0;

static const char *state_name(led_state_t s)
{
    switch (s) {
    case LED_STATE_OFF:           return "off";
    case LED_STATE_PROVISIONING:  return "provisioning";
    case LED_STATE_CONNECTING:    return "connecting";
    case LED_STATE_NEGOTIATING:   return "negotiating";
    case LED_STATE_LISTENING:     return "listening";
    case LED_STATE_TALKING:       return "talking";
    case LED_STATE_THINKING:      return "thinking";
    case LED_STATE_WAKE_ACK:      return "wake!";
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
    int64_t now = esp_timer_get_time();
    // Honour the wake-ack hold: subsequent state changes (LISTENING from
    // the playback task, etc.) are deferred until the flash window elapses.
    if (s != LED_STATE_WAKE_ACK && now < s_hold_until_us) return;
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
        xvf3800_set_led_effect(EFFECT_BREATH);
        break;
    case LED_STATE_CONNECTING:
        xvf3800_set_led_color(RGB_BLUE);
        xvf3800_set_led_speed(0x80);            // brisk breath
        xvf3800_set_led_effect(EFFECT_BREATH);
        break;
    case LED_STATE_NEGOTIATING:
        // Distinct from CONNECTING so the user can tell they're stuck in
        // ICE/DTLS specifically. Purple at a moderate breath rate.
        xvf3800_set_led_color(RGB_PURPLE);
        xvf3800_set_led_speed(0x60);
        xvf3800_set_led_effect(EFFECT_BREATH);
        break;
    case LED_STATE_LISTENING:
        xvf3800_set_led_color(RGB_GREEN);
        xvf3800_set_led_effect(EFFECT_SOLID);
        break;
    case LED_STATE_TALKING:
        // Cyan solid while the user is speaking — distinct enough from
        // LISTENING green that "I see you speaking" is unambiguous, but
        // similar enough in hue that it doesn't feel like an error state.
        xvf3800_set_led_color(RGB_CYAN);
        xvf3800_set_led_effect(EFFECT_SOLID);
        break;
    case LED_STATE_THINKING:
        // Amber breath while the backend round-trip is in flight. Slow
        // breath conveys "working" without being urgent.
        xvf3800_set_led_color(RGB_AMBER);
        xvf3800_set_led_speed(0x40);
        xvf3800_set_led_effect(EFFECT_BREATH);
        break;
    case LED_STATE_WAKE_ACK:
        // Bright white flash, held for WAKE_ACK_HOLD_MS so the playback
        // task's post-TTS LISTENING set doesn't immediately mask it.
        xvf3800_set_led_color(RGB_WHITE);
        xvf3800_set_led_brightness(0xFF);
        xvf3800_set_led_effect(EFFECT_SOLID);
        s_hold_until_us = now + WAKE_ACK_HOLD_MS * 1000;
        break;
    case LED_STATE_SPEAKING:
        // Pink solid while TTS plays. Tried EFFECT_DOA (rotating segment
        // following the loudest mic direction) but it ends up tracking
        // the device's own speaker output and drifts unpredictably —
        // solid is cleaner and the colour change alone (vs LISTENING
        // green) is enough of an at-a-glance signal.
        xvf3800_set_led_color(RGB_PINK);
        xvf3800_set_led_effect(EFFECT_SOLID);
        break;
    case LED_STATE_MUTED:
        xvf3800_set_led_color(RGB_RED);
        xvf3800_set_led_effect(EFFECT_SOLID);
        break;
    case LED_STATE_ERROR:
        xvf3800_set_led_color(RGB_RED);
        xvf3800_set_led_speed(0xC0);            // urgent
        xvf3800_set_led_effect(EFFECT_BREATH);
        break;
    }
}

// ---------- Live-conversation state machine -------------------------------
//
// Hold timings. SPEAKING bridges natural inter-word silences inside a TTS
// reply (comma in "Привет, Кирилл" is ~300 ms). TALKING bridges sub-syllable
// silences in user speech. THINKING window starts when TALKING releases and
// closes when SPEAKING starts or when the window times out without a reply.
#define SPEAKING_HOLD_MS    1500
#define TALKING_HOLD_MS      600
#define THINKING_MAX_MS     5000

led_state_t leds_session_tick(const led_session_inputs_t *inputs)
{
    static led_state_t s_last_pushed = LED_STATE_OFF;

    if (!inputs->connected) {
        // Connecting / negotiating LEDs are driven by webrtc_session's
        // peer-state callback; don't fight it from here.
        return s_last_pushed;
    }

    TickType_t now = inputs->now_tick;
    bool speaking = (now - inputs->last_rx_frame_tick) <
                    pdMS_TO_TICKS(SPEAKING_HOLD_MS);
    bool talking  = !speaking &&
                    (now - inputs->last_mic_active_tick) <
                        pdMS_TO_TICKS(TALKING_HOLD_MS);
    bool thinking = !talking && !speaking &&
                    ((now - inputs->last_mic_active_tick) <
                        pdMS_TO_TICKS(TALKING_HOLD_MS + THINKING_MAX_MS));

    led_state_t want = inputs->muted ? LED_STATE_MUTED
                     : speaking      ? LED_STATE_SPEAKING
                     : talking       ? LED_STATE_TALKING
                     : thinking      ? LED_STATE_THINKING
                                     : LED_STATE_LISTENING;
    if (want != s_last_pushed) {
        leds_set(want);
        s_last_pushed = want;
    }
    return want;
}

