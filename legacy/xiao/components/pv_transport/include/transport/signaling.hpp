#pragma once

// HTTPS signaling against the pipecat backend.
//
//   POST   {base}/api/offer    {"sdp": "...", "type": "offer"}
//                            → {"pc_id": "...", "sdp": "...", "type": "answer"}
//   GET    {base}/ice-servers → {"iceServers": [{url, username, credential}, ...]}
//
// libpeer does non-trickle ICE (every local candidate is inlined into
// the initial offer), so there's no PATCH path on the device side.
// Browser clients still use it but that lives in the backend, not here.

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "esp_err.h"
#include "hal/https_client.hpp"

namespace transport {

struct IceServer {
    std::string url;
    std::string username;
    std::string credential;
};

struct OfferResponse {
    std::string pc_id;
    std::string remote_sdp;
};

class Signaling {
public:
    explicit Signaling(std::string base_url);

    // POST /api/offer. Returns std::nullopt on transport / HTTP / JSON
    // failure (logged at ERROR level).
    std::optional<OfferResponse> sendOffer(std::string_view local_sdp);

    // GET /ice-servers. Empty vector if backend has no TURN configured
    // (degrades to host-candidate-only ICE).
    std::vector<IceServer> fetchIceServers();

    // pc_id from the most recent successful sendOffer(). Useful for
    // log correlation; empty before first success.
    const std::string& pcId() const { return pc_id_; }

private:
    std::string      base_url_;
    std::string      offer_url_;
    std::string      ice_url_;
    std::string      pc_id_;
    hal::HttpsClient http_{16'384};
};

} // namespace transport
