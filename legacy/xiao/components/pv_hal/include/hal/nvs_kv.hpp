#pragma once

// Tiny string-valued KV abstraction over ESP-IDF's NVS. Used by the
// app for Wi-Fi credentials + backend URL. Each NvsKv handle owns one
// open NVS handle (RAII-closed on destruction).
//
// Why typed-and-stringly rather than a general "Value" variant: every
// key we store today is a string, and the cost of a bigger API outweighs
// the value. If we ever need uint32_t etc. add a method here, don't
// reach into nvs.h from the call sites.

#include <optional>
#include <string>
#include <string_view>

#include "esp_err.h"
#include "nvs.h"

namespace hal {

class NvsKv {
public:
    // Open the namespace read-write. Returns nullopt on NVS init / open
    // failure. NVS partition is the default; call NvsKv::initPartition()
    // once at app startup before any open().
    static std::optional<NvsKv> open(const char* namespace_name);

    // Initialise the default NVS partition. Idempotent; safe to call
    // many times. Handles the "no free pages" / "new version" cases by
    // erasing and reinitialising — destroys any persisted data, so
    // call this exactly once at startup before any NvsKv::open().
    static esp_err_t initPartition();

    NvsKv(const NvsKv&)            = delete;
    NvsKv& operator=(const NvsKv&) = delete;
    NvsKv(NvsKv&& other) noexcept;
    NvsKv& operator=(NvsKv&& other) noexcept;
    ~NvsKv();

    // Get a string. nullopt if key absent.
    std::optional<std::string> getStr(const char* key) const;

    esp_err_t setStr(const char* key, std::string_view value);
    esp_err_t erase(const char* key);
    esp_err_t commit();

private:
    explicit NvsKv(nvs_handle_t h) noexcept : h_(h) {}
    nvs_handle_t h_{0};
    bool         valid_{false};
};

} // namespace hal
