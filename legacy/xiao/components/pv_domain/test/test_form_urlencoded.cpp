#include <catch2/catch_test_macros.hpp>

#include "domain/form_urlencoded.hpp"

using namespace domain;

TEST_CASE("urlDecode basic ASCII pass-through", "[form_urlencoded]") {
    REQUIRE(urlDecode("hello") == "hello");
    REQUIRE(urlDecode("") == "");
}

TEST_CASE("urlDecode +  →  space", "[form_urlencoded]") {
    REQUIRE(urlDecode("hello+world") == "hello world");
}

TEST_CASE("urlDecode percent escapes", "[form_urlencoded]") {
    REQUIRE(urlDecode("a%20b") == "a b");
    REQUIRE(urlDecode("a%2Bb") == "a+b");
    REQUIRE(urlDecode("%C3%BC") == "\xc3\xbc");   // UTF-8 'ü'
}

TEST_CASE("urlDecode malformed escape leaves % literal", "[form_urlencoded]") {
    REQUIRE(urlDecode("a%xy") == "a%xy");        // bad hex
    REQUIRE(urlDecode("a%2") == "a%2");          // truncated
}

TEST_CASE("formGet missing key", "[form_urlencoded]") {
    REQUIRE_FALSE(formGet("ssid=foo&psk=bar", "backend").has_value());
}

TEST_CASE("formGet first field", "[form_urlencoded]") {
    auto v = formGet("ssid=foo&psk=bar", "ssid");
    REQUIRE(v.has_value());
    REQUIRE(*v == "foo");
}

TEST_CASE("formGet middle field", "[form_urlencoded]") {
    auto v = formGet("a=1&ssid=foo&b=2", "ssid");
    REQUIRE(*v == "foo");
}

TEST_CASE("formGet last field (no trailing &)", "[form_urlencoded]") {
    auto v = formGet("a=1&ssid=foo", "ssid");
    REQUIRE(*v == "foo");
}

TEST_CASE("formGet decoded value", "[form_urlencoded]") {
    auto v = formGet("ssid=foo%20bar&psk=p%40ssword", "psk");
    REQUIRE(*v == "p@ssword");
}

TEST_CASE("formGet empty value", "[form_urlencoded]") {
    auto v = formGet("ssid=&psk=bar", "ssid");
    REQUIRE(v.has_value());
    REQUIRE(*v == "");
}

TEST_CASE("formGet does not match key prefix", "[form_urlencoded]") {
    // "ssid_extra" is not "ssid"; must return nullopt.
    REQUIRE_FALSE(formGet("ssid_extra=foo", "ssid").has_value());
}
