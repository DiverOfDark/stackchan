#include "hal/button.hpp"

#include <utility>

#include "esp_log.h"
#include "iot_button.h"
#include "button_gpio.h"

namespace {

constexpr const char* kTag         = "button";
constexpr int         kLongPressMs = 5000;

} // namespace

namespace hal {

// Static thunks that the iot_button C API can call. `arg` is the
// Button instance; we recover it and forward to instance state. The
// Button class declares ButtonInternal a friend so we can touch
// muted_ + long_press_ without exposing them through the header.
struct ButtonInternal {
    static void shortPress(void* arg, void* /*usr*/)
    {
        auto* self = static_cast<Button*>(arg);
        self->muted_ = !self->muted_;
        ESP_LOGI(kTag, "short press → muted=%d", self->muted_);
    }
    static void longPress(void* arg, void* /*usr*/)
    {
        auto* self = static_cast<Button*>(arg);
        if (self->long_press_) self->long_press_();
    }
};

std::optional<Button> Button::create(int gpio)
{
    button_config_t btn_cfg{};
    button_gpio_config_t gpio_cfg{};
    gpio_cfg.gpio_num     = gpio;
    gpio_cfg.active_level = 0;     // active-low

    button_handle_t h = nullptr;
    esp_err_t err = iot_button_new_gpio_device(&btn_cfg, &gpio_cfg, &h);
    if (err != ESP_OK || !h) {
        ESP_LOGE(kTag, "iot_button_new_gpio_device failed: %s", esp_err_to_name(err));
        return std::nullopt;
    }
    return Button{h};
}

Button::Button(Button&& other) noexcept
    : handle_(other.handle_), muted_(other.muted_),
      long_press_(std::move(other.long_press_))
{
    other.handle_ = nullptr;
}

Button& Button::operator=(Button&& other) noexcept
{
    if (this != &other) {
        if (handle_) iot_button_delete(static_cast<button_handle_t>(handle_));
        handle_     = other.handle_;
        muted_      = other.muted_;
        long_press_ = std::move(other.long_press_);
        other.handle_ = nullptr;
    }
    return *this;
}

Button::~Button()
{
    if (handle_) iot_button_delete(static_cast<button_handle_t>(handle_));
}

void Button::setOnLongPress(LongPressFn fn)
{
    long_press_ = std::move(fn);
    if (!handle_) return;
    // (Re-)register callbacks. iot_button doesn't expose an "update"
    // call, so we unregister + register fresh. Both register calls
    // pass `this` as `arg` so the static thunks can recover the Button.
    iot_button_unregister_cb(static_cast<button_handle_t>(handle_),
                             BUTTON_SINGLE_CLICK, nullptr);
    iot_button_unregister_cb(static_cast<button_handle_t>(handle_),
                             BUTTON_LONG_PRESS_START, nullptr);

    iot_button_register_cb(static_cast<button_handle_t>(handle_),
                           BUTTON_SINGLE_CLICK, nullptr,
                           ButtonInternal::shortPress, this);

    button_event_args_t long_args{};
    long_args.long_press.press_time = kLongPressMs;
    iot_button_register_cb(static_cast<button_handle_t>(handle_),
                           BUTTON_LONG_PRESS_START, &long_args,
                           ButtonInternal::longPress, this);
}

} // namespace hal
