#pragma once

// Tiny HTTPS request helper around esp_http_client. One-shot
// request/response model: build a Request, call perform(), get back
// the body + status. The mbedTLS context lives for the duration of
// the call and is torn down with esp_http_client_cleanup().
//
// JSON construction / parsing is the caller's problem — typically
// transport::Signaling, which uses cJSON.

#include <cstdint>
#include <string>
#include <string_view>

#include "esp_err.h"
#include "esp_http_client.h"

namespace hal {

struct HttpResponse {
    int         status = 0;
    std::string body;
};

class HttpsClient {
public:
    // Configure the response buffer cap. Captive-portal-style fetches
    // (ICE servers JSON) sit under 4 KB; SDP answers can run 8-16 KB.
    explicit HttpsClient(std::size_t max_response_bytes = 16'384)
        : cap_(max_response_bytes) {}

    // Performs HTTPS request synchronously. esp-tls + esp_crt_bundle.
    // method:   GET / POST / PATCH (esp_http_client_method_t)
    // body:     null or empty for GET; otherwise sent with the
    //           Content-Type header below.
    esp_err_t request(const char* url,
                      esp_http_client_method_t method,
                      std::string_view body,
                      const char* content_type,    // e.g. "application/json"; null for GET
                      HttpResponse& out);

private:
    std::size_t cap_;
};

} // namespace hal
