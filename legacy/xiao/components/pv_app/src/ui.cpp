#include "app/ui.hpp"

#include "esp_log.h"
#include "esp_timer.h"

namespace {

constexpr const char* kTag             = "ui";
constexpr int64_t     kWakeAckHoldMs   = 1200;

// Colour palette in RGB. Tuned for daylight visibility on the diffused
// 12-LED ring of the Seeed XVF3800 carrier.
constexpr hal::Rgb kBlue   {0x00, 0x80, 0xFF};
constexpr hal::Rgb kGreen  {0x00, 0xC8, 0x40};
constexpr hal::Rgb kRed    {0xFF, 0x20, 0x10};
constexpr hal::Rgb kWhite  {0xFF, 0xFF, 0xFF};
constexpr hal::Rgb kPink   {0xFF, 0x30, 0x80};
constexpr hal::Rgb kPurple {0x80, 0x20, 0xFF};
constexpr hal::Rgb kCyan   {0x00, 0xC0, 0xFF};
constexpr hal::Rgb kAmber  {0xFF, 0x88, 0x00};

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
    ring_.setBrightness(0x60);
    ring_.setSpeed(0x40);
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

    using L  = domain::LedState;
    using EF = hal::Effect;
    switch (s) {
    case L::Off:
        ring_.setEffect(EF::Off);
        break;
    case L::Provisioning:
        ring_.setColor(kBlue);   ring_.setSpeed(0x20);  ring_.setEffect(EF::Breath);
        break;
    case L::Connecting:
        ring_.setColor(kBlue);   ring_.setSpeed(0x80);  ring_.setEffect(EF::Breath);
        break;
    case L::Negotiating:
        ring_.setColor(kPurple); ring_.setSpeed(0x60);  ring_.setEffect(EF::Breath);
        break;
    case L::Listening:
        ring_.setColor(kGreen);  ring_.setEffect(EF::Solid);
        break;
    case L::Talking:
        ring_.setColor(kCyan);   ring_.setEffect(EF::Solid);
        break;
    case L::Thinking:
        ring_.setColor(kAmber);  ring_.setSpeed(0x40);  ring_.setEffect(EF::Breath);
        break;
    case L::WakeAck:
        ring_.setColor(kWhite);  ring_.setBrightness(0xFF); ring_.setEffect(EF::Solid);
        hold_until_us_ = now + kWakeAckHoldMs * 1000;
        break;
    case L::Speaking:
        ring_.setColor(kPink);   ring_.setEffect(EF::Solid);
        break;
    case L::Muted:
        ring_.setColor(kRed);    ring_.setEffect(EF::Solid);
        break;
    case L::Error:
        ring_.setColor(kRed);    ring_.setSpeed(0xC0);  ring_.setEffect(EF::Breath);
        break;
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
