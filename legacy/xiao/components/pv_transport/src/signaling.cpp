#include "transport/signaling.hpp"

#include "cJSON.h"
#include "esp_log.h"

namespace {

constexpr const char* kTag       = "signaling";
constexpr const char* kOfferPath = "/api/offer";
constexpr const char* kIcePath   = "/ice-servers";

// Join "https://host.com" + "/path" — trim any trailing '/' from base
// and any leading from path so the result is canonical.
std::string joinUrl(std::string_view base, std::string_view path)
{
    while (!base.empty() && base.back() == '/')     base.remove_suffix(1);
    while (!path.empty() && path.front() == '/')    path.remove_prefix(1);
    std::string out;
    out.reserve(base.size() + 1 + path.size());
    out.append(base);
    out.push_back('/');
    out.append(path);
    return out;
}

} // namespace

namespace transport {

Signaling::Signaling(std::string base_url)
    : base_url_(std::move(base_url))
    , offer_url_(joinUrl(base_url_, kOfferPath))
    , ice_url_(joinUrl(base_url_, kIcePath))
{}

std::optional<OfferResponse> Signaling::sendOffer(std::string_view local_sdp)
{
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "type", "offer");
    cJSON_AddStringToObject(req, "sdp",  std::string(local_sdp).c_str());
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return std::nullopt;

    hal::HttpResponse resp;
    esp_err_t err = http_.request(offer_url_.c_str(), HTTP_METHOD_POST,
                                  body, "application/json", resp);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(kTag, "/api/offer transport err: %s", esp_err_to_name(err));
        return std::nullopt;
    }
    if (resp.status / 100 != 2) {
        ESP_LOGE(kTag, "/api/offer status=%d body=%.*s",
                 resp.status, static_cast<int>(resp.body.size()), resp.body.data());
        return std::nullopt;
    }

    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) {
        ESP_LOGE(kTag, "/api/offer: malformed JSON response");
        return std::nullopt;
    }
    OfferResponse out;
    cJSON* sdp   = cJSON_GetObjectItem(root, "sdp");
    cJSON* pc_id = cJSON_GetObjectItem(root, "pc_id");
    if (cJSON_IsString(sdp) && cJSON_IsString(pc_id)) {
        out.remote_sdp = sdp->valuestring;
        out.pc_id      = pc_id->valuestring;
        pc_id_         = out.pc_id;
    }
    cJSON_Delete(root);

    if (out.pc_id.empty() || out.remote_sdp.empty()) {
        ESP_LOGE(kTag, "/api/offer: response missing sdp / pc_id");
        return std::nullopt;
    }
    ESP_LOGI(kTag, "offer accepted, pc_id=%s sdp_len=%u",
             out.pc_id.c_str(), (unsigned)out.remote_sdp.size());
    return out;
}

std::vector<IceServer> Signaling::fetchIceServers()
{
    std::vector<IceServer> out;
    hal::HttpsClient slim{4096};
    hal::HttpResponse resp;
    esp_err_t err = slim.request(ice_url_.c_str(), HTTP_METHOD_GET,
                                 {}, /*content_type=*/nullptr, resp);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "/ice-servers transport err: %s", esp_err_to_name(err));
        return out;
    }
    if (resp.status / 100 != 2) {
        ESP_LOGW(kTag, "/ice-servers status=%d", resp.status);
        return out;
    }
    cJSON* root = cJSON_Parse(resp.body.c_str());
    if (!root) return out;
    cJSON* arr = cJSON_GetObjectItem(root, "iceServers");
    if (cJSON_IsArray(arr)) {
        const int n = cJSON_GetArraySize(arr);
        out.reserve(n);
        for (int i = 0; i < n; ++i) {
            cJSON* item = cJSON_GetArrayItem(arr, i);
            cJSON* u    = cJSON_GetObjectItem(item, "urls");
            cJSON* user = cJSON_GetObjectItem(item, "username");
            cJSON* cred = cJSON_GetObjectItem(item, "credential");
            IceServer s;
            if (cJSON_IsString(u))    s.url        = u->valuestring;
            if (cJSON_IsString(user)) s.username   = user->valuestring;
            if (cJSON_IsString(cred)) s.credential = cred->valuestring;
            if (!s.url.empty()) out.push_back(std::move(s));
        }
    }
    cJSON_Delete(root);
    return out;
}

} // namespace transport
