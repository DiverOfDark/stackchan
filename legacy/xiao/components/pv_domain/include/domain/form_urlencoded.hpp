#pragma once

// Minimal application/x-www-form-urlencoded value extractor + URL
// decoder. Used by the SoftAP captive-portal handler to pull the SSID
// / PSK / backend URL fields out of the POST body without dragging
// any ESP-IDF code into the parsing logic.

#include <optional>
#include <string>
#include <string_view>

namespace domain {

// Decode an x-www-form-urlencoded value (per WHATWG URL spec): '+' →
// space, '%XX' → byte, everything else copied as-is. Pure function;
// host-testable. Returns the decoded string by value (kept on the
// stack at our captive-portal payload sizes).
std::string urlDecode(std::string_view in);

// Find `key` in an x-www-form-urlencoded `body` (e.g. "ssid=Foo&psk=bar")
// and return the URL-decoded value. std::nullopt if the key isn't
// present.
std::optional<std::string> formGet(std::string_view body, std::string_view key);

} // namespace domain
