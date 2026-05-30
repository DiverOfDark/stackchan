#pragma once

// Thin wrapper over the espressif/mdns component so the device answers at
// <hostname>.local. It lives in pv_hal deliberately: libpeer ships its own
// src/mdns.h on the include path, so a plain #include "mdns.h" in any
// component that (transitively) requires libpeer resolves to the WRONG header.
// pv_hal doesn't depend on libpeer, so here "mdns.h" resolves correctly — and
// callers (main, web server) just use this wrapper.

#include <string>

namespace hal {

class Mdns {
public:
    // Initialise mDNS, set the hostname, and advertise the HTTP service on :80.
    // Returns false if mdns_init fails (name resolution just won't work; IP does).
    static bool start(const std::string& hostname);

    // Change the advertised hostname at runtime.
    static void setHostname(const std::string& hostname);
};

} // namespace hal
