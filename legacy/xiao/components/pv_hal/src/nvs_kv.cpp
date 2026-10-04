#include "hal/nvs_kv.hpp"

#include "esp_log.h"
#include "nvs_flash.h"

namespace {
constexpr const char* kTag = "nvs_kv";
} // namespace

namespace hal {

esp_err_t NvsKv::initPartition()
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(kTag, "NVS partition needs erase (%s)", esp_err_to_name(err));
        if (nvs_flash_erase() == ESP_OK) {
            err = nvs_flash_init();
        }
    }
    return err;
}

std::optional<NvsKv> NvsKv::open(const char* namespace_name)
{
    nvs_handle_t h = 0;
    esp_err_t err = nvs_open(namespace_name, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "nvs_open(%s) failed: %s", namespace_name, esp_err_to_name(err));
        return std::nullopt;
    }
    NvsKv k{h};
    k.valid_ = true;
    return k;
}

NvsKv::NvsKv(NvsKv&& other) noexcept : h_(other.h_), valid_(other.valid_)
{
    other.valid_ = false;
}

NvsKv& NvsKv::operator=(NvsKv&& other) noexcept
{
    if (this != &other) {
        if (valid_) nvs_close(h_);
        h_     = other.h_;
        valid_ = other.valid_;
        other.valid_ = false;
    }
    return *this;
}

NvsKv::~NvsKv()
{
    if (valid_) nvs_close(h_);
}

std::optional<std::string> NvsKv::getStr(const char* key) const
{
    if (!valid_) return std::nullopt;
    std::size_t len = 0;
    if (nvs_get_str(h_, key, nullptr, &len) != ESP_OK || len == 0) {
        return std::nullopt;
    }
    std::string out;
    out.resize(len);          // len includes the trailing '\0'
    if (nvs_get_str(h_, key, out.data(), &len) != ESP_OK) {
        return std::nullopt;
    }
    if (!out.empty() && out.back() == '\0') out.pop_back();
    return out;
}

esp_err_t NvsKv::setStr(const char* key, std::string_view value)
{
    if (!valid_) return ESP_ERR_INVALID_STATE;
    // NVS API needs a null-terminated buffer; copy to a temporary
    // string. Captive-portal values are short (< 100 B) — cheap.
    std::string tmp(value);
    return nvs_set_str(h_, key, tmp.c_str());
}

esp_err_t NvsKv::erase(const char* key)
{
    if (!valid_) return ESP_ERR_INVALID_STATE;
    return nvs_erase_key(h_, key);
}

esp_err_t NvsKv::commit()
{
    if (!valid_) return ESP_ERR_INVALID_STATE;
    return nvs_commit(h_);
}

} // namespace hal
