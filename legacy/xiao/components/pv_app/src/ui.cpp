#include "app/ui.hpp"

#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr const char* kTag             = "ui";
constexpr int64_t     kWakeAckHoldMs   = 1200;

// One consistent brightness + speed for every state — only the effect and
// colour distinguish them (a deliberate design choice). 0x80 ≈ 50%.
// NOTE: the XVF3800 only applies LED_BRIGHTNESS / LED_SPEED to the breath and
// rainbow effects; single-colour mode ignores them, so for the solid states
// the 50 % level is baked into the RGB (each lit channel ≈ 0x80) instead.
constexpr uint8_t  kBrightness = 0x80;
constexpr uint8_t  kSpeed      = 0x40;

// Breath hues — full intensity; LED_BRIGHTNESS scales the pulse to 50 %.
constexpr hal::Rgb kBreathBlue   {0x00, 0x40, 0xFF};   // connecting
constexpr hal::Rgb kBreathPurple {0x90, 0x00, 0xFF};   // negotiating
constexpr hal::Rgb kBreathAmber  {0xFF, 0x80, 0x00};   // thinking
constexpr hal::Rgb kBreathPink   {0xFF, 0x10, 0x80};   // speaking
constexpr hal::Rgb kBreathRed    {0xFF, 0x00, 0x00};   // error

// Solid colours — 50 % baked into the RGB (single-colour mode ignores
// brightness). Each is a distinct hue from the breath set above.
constexpr hal::Rgb kSolidGreen   {0x00, 0x80, 0x10};   // listening
constexpr hal::Rgb kSolidCyan    {0x00, 0x80, 0x80};   // talking
constexpr hal::Rgb kSolidWhite   {0x80, 0x80, 0x80};   // wake ack
constexpr hal::Rgb kSolidRed     {0x80, 0x00, 0x00};   // muted

const char* stateName(domain::LedState s)
{
    using L = domain::LedState;
    switch (s) {
    case L::Off:           return "off";
    case L::Provisioning:  return "provisioning";
    case L::Connecting:    return "connecting";
    case L::Negotiating:   return "negotiating";
    case L::Listening:     return "listening";
    case L::Talking:       return "talking";
    case L::Thinking:      return "thinking";
    case L::WakeAck:       return "wake!";
    case L::Speaking:      return "speaking";
    case L::Muted:         return "muted";
    case L::Error:         return "error";
    }
    return "?";
}

} // namespace

namespace app {

Ui::Ui(hal::Xvf3800& ring) : ring_(ring)
{
    ring_.setBrightness(kBrightness);
    ring_.setSpeed(kSpeed);
    ring_.setEffect(hal::Effect::Off);
}

void Ui::setLed(domain::LedState s)
{
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t now = esp_timer_get_time();
    // A web LED-test override owns the ring; ignore state-machine updates.
    if (overrideActive(now)) return;
    // Honour the wake-ack hold (subsequent live ticks should not
    // immediately mask a fresh wake flash).
    if (s != domain::LedState::WakeAck && now < hold_until_us_) return;
    if (!force_ && s == last_pushed_) return;
    force_ = false;
    applyState(s, now);
}

void Ui::applyState(domain::LedState s, const int64_t now)
{
    last_pushed_ = s;
    ESP_LOGI(kTag, "→ %s", stateName(s));

    // Consistent for every state — states differ ONLY by effect + colour.
    // (No-ops for solid/off, which ignore brightness+speed, but cheap and
    // keeps levels correct after the web LED-test tool has tweaked them.)
    ring_.setBrightness(kBrightness);
    ring_.setSpeed(kSpeed);

    using L  = domain::LedState;
    using EF = hal::Effect;
    switch (s) {
    case L::Off:           ring_.setEffect(EF::Off);                                    break;
    case L::Provisioning:  ring_.setEffect(EF::Rainbow);                                break;
    case L::Connecting:    ring_.setColor(kBreathBlue);   ring_.setEffect(EF::Breath);  break;
    case L::Negotiating:   ring_.setColor(kBreathPurple); ring_.setEffect(EF::Breath);  break;
    case L::Listening:     ring_.setColor(kSolidGreen);   ring_.setEffect(EF::Solid);   break;
    case L::Talking:       ring_.setColor(kSolidCyan);    ring_.setEffect(EF::Solid);   break;
    case L::Thinking:      ring_.setColor(kBreathAmber);  ring_.setEffect(EF::Breath);  break;
    case L::WakeAck:
        ring_.setColor(kSolidWhite); ring_.setEffect(EF::Solid);
        hold_until_us_ = now + kWakeAckHoldMs * 1000;
        break;
    case L::Speaking:      ring_.setColor(kBreathPink);   ring_.setEffect(EF::Breath);  break;
    case L::Muted:         ring_.setColor(kSolidRed);     ring_.setEffect(EF::Solid);   break;
    case L::Error:         ring_.setColor(kBreathRed);    ring_.setEffect(EF::Breath);  break;
    }
}

bool Ui::overrideActive(const int64_t now)
{
    if (override_until_us_ == 0) return false;
    if (now < override_until_us_) return true;
    // Override just expired — resume the state machine and force the next
    // setLed() to re-apply even if the computed state equals last_pushed_.
    override_until_us_ = 0;
    force_ = true;
    return false;
}

void Ui::overrideEffect(hal::Effect e, hal::Rgb colour,
                        uint8_t brightness, uint8_t speed, int hold_ms)
{
    std::lock_guard<std::mutex> lk(mu_);
    override_until_us_ = esp_timer_get_time() + static_cast<int64_t>(hold_ms) * 1000;
    ring_.setColor(colour);
    ring_.setBrightness(brightness);
    ring_.setSpeed(speed);
    ring_.setEffect(e);
}

void Ui::overrideState(domain::LedState s, int hold_ms)
{
    std::lock_guard<std::mutex> lk(mu_);
    const int64_t now = esp_timer_get_time();
    override_until_us_ = now + static_cast<int64_t>(hold_ms) * 1000;
    applyState(s, now);   // drive the real mapping directly
}

void Ui::resumeAuto()
{
    std::lock_guard<std::mutex> lk(mu_);
    override_until_us_ = 0;
    force_ = true;
}

void Ui::tick(const domain::LedInputs& in)
{
    auto next = domain::resolveLedState(in);
    if (next.has_value()) setLed(*next);
}

} // namespace app
