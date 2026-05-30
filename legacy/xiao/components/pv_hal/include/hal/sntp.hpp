#pragma once

// Wall-clock sync via SNTP, with a build-date fallback so TLS works on
// flaky networks. Standalone class — split out of the old
// wifi_provision.c which mixed time-sync into the Wi-Fi STA path.
//
// Why this matters: mbedTLS rejects "notBefore: 2024-01-01" as a
// not-yet-valid certificate when the RTC sits at 1970 (the chip's
// default after a cold boot). We need a plausible "now" before the
// first HTTPS request.

#include <chrono>

namespace hal {

class Sntp {
public:
    // Try SNTP; if it doesn't respond within `timeout`, fall back to
    // the firmware build date (so we drift slowly forward across
    // rebuilds rather than staying stuck at 1970).
    //
    // Static: there's never more than one SNTP client per process,
    // and the ESP-IDF API is global. No RAII required — once started
    // the client keeps refining the time in the background.
    static void syncOrFallback(std::chrono::milliseconds timeout = std::chrono::seconds{10});
};

} // namespace hal
