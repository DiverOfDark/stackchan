#include "pipecat_signaling.h"

#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"

static const char *TAG = "signaling";

#define OFFER_PATH       "/api/offer"
#define ICE_PATH         "/ice-servers"
#define HTTP_TIMEOUT_MS  15000
#define MAX_RESPONSE     16384   // SDP answers run a few KB
#define MAX_ICE_RESPONSE 4096    // /ice-servers payload is small

struct pipecat_signaling {
    const char *base_url;   // not owned; borrowed from caller (NVS-backed)
    char       *offer_url;  // owned
    char       *pc_id;      // owned, populated after offer
};

// ---------- Response accumulator -----------------------------------------

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} resp_buf_t;

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_DATA || evt->user_data == NULL) {
        return ESP_OK;
    }
    resp_buf_t *rb = (resp_buf_t *)evt->user_data;
    if (rb->len + evt->data_len + 1 > rb->cap) {
        ESP_LOGE(TAG, "response > %u bytes — truncating", (unsigned)rb->cap);
        return ESP_FAIL;
    }
    memcpy(rb->buf + rb->len, evt->data, evt->data_len);
    rb->len += evt->data_len;
    rb->buf[rb->len] = '\0';
    return ESP_OK;
}

// ---------- Internal HTTP request helper ---------------------------------

static esp_err_t do_request(const char *url, esp_http_client_method_t method,
                            const char *body, char *resp_buf, size_t resp_cap,
                            int *out_status)
{
    resp_buf_t rb = { .buf = resp_buf, .len = 0, .cap = resp_cap };
    if (resp_buf && resp_cap > 0) resp_buf[0] = '\0';

    esp_http_client_config_t cfg = {
        .url           = url,
        .method        = method,
        .event_handler = http_event_cb,
        .user_data     = &rb,
        .timeout_ms    = HTTP_TIMEOUT_MS,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t cli = esp_http_client_init(&cfg);
    if (!cli) return ESP_ERR_NO_MEM;

    esp_http_client_set_header(cli, "Content-Type", "application/json");
    if (body) {
        esp_http_client_set_post_field(cli, body, strlen(body));
    }

    esp_err_t err = esp_http_client_perform(cli);
    if (out_status) *out_status = esp_http_client_get_status_code(cli);
    esp_http_client_cleanup(cli);
    return err;
}

// ---------- Public API ---------------------------------------------------

pipecat_signaling_t *pipecat_signaling_create(const char *base_url)
{
    if (!base_url || !*base_url) return NULL;
    pipecat_signaling_t *sig = calloc(1, sizeof(*sig));
    if (!sig) return NULL;

    sig->base_url = base_url;

    // Trim trailing slash from base, then append "/api/offer".
    size_t base_len = strlen(base_url);
    while (base_len > 0 && base_url[base_len - 1] == '/') base_len--;
    size_t need = base_len + sizeof(OFFER_PATH); // includes NUL
    sig->offer_url = malloc(need);
    if (!sig->offer_url) {
        free(sig);
        return NULL;
    }
    memcpy(sig->offer_url, base_url, base_len);
    memcpy(sig->offer_url + base_len, OFFER_PATH, sizeof(OFFER_PATH));
    return sig;
}

void pipecat_signaling_destroy(pipecat_signaling_t *sig)
{
    if (!sig) return;
    free(sig->offer_url);
    free(sig->pc_id);
    free(sig);
}

esp_err_t pipecat_signaling_send_offer(pipecat_signaling_t *sig,
                                       const char *local_sdp,
                                       char **out_remote_sdp)
{
    if (!sig || !local_sdp || !out_remote_sdp) return ESP_ERR_INVALID_ARG;
    *out_remote_sdp = NULL;

    cJSON *req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "sdp",  local_sdp);
    cJSON_AddStringToObject(req, "type", "offer");
    char *body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) return ESP_ERR_NO_MEM;

    char *resp = malloc(MAX_RESPONSE);
    if (!resp) { free(body); return ESP_ERR_NO_MEM; }

    int status = 0;
    esp_err_t err = do_request(sig->offer_url, HTTP_METHOD_POST,
                               body, resp, MAX_RESPONSE, &status);
    free(body);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "offer POST failed: %s", esp_err_to_name(err));
        free(resp);
        return err;
    }
    if (status / 100 != 2) {
        ESP_LOGE(TAG, "offer POST status=%d body=%s", status, resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        ESP_LOGE(TAG, "offer response is not JSON");
        return ESP_FAIL;
    }
    const cJSON *sdp   = cJSON_GetObjectItem(root, "sdp");
    const cJSON *pc_id = cJSON_GetObjectItem(root, "pc_id");
    if (!cJSON_IsString(sdp) || !cJSON_IsString(pc_id)) {
        ESP_LOGE(TAG, "offer response missing sdp/pc_id");
        cJSON_Delete(root);
        return ESP_FAIL;
    }

    free(sig->pc_id);
    sig->pc_id = strdup(pc_id->valuestring);
    *out_remote_sdp = strdup(sdp->valuestring);
    cJSON_Delete(root);

    if (!sig->pc_id || !*out_remote_sdp) {
        free(sig->pc_id);  sig->pc_id = NULL;
        free(*out_remote_sdp); *out_remote_sdp = NULL;
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "offer accepted, pc_id=%s sdp_len=%u",
             sig->pc_id, (unsigned)strlen(*out_remote_sdp));
    return ESP_OK;
}

// ---------- ICE server discovery -----------------------------------------

void pipecat_signaling_free_ice_servers(pipecat_ice_server_t *servers,
                                        size_t count)
{
    if (!servers) return;
    for (size_t i = 0; i < count; ++i) {
        free(servers[i].url);
        free(servers[i].username);
        free(servers[i].credential);
    }
    free(servers);
}

// Extract the first URL from a cJSON node that may be either a string
// ("turn:..." ) or an array of strings (["turn:...", "turns:..."]). esp_peer
// can only use one URL per server entry; first wins.
static char *extract_first_url(const cJSON *urls)
{
    if (cJSON_IsString(urls)) return strdup(urls->valuestring);
    if (cJSON_IsArray(urls) && cJSON_GetArraySize(urls) > 0) {
        const cJSON *first = cJSON_GetArrayItem(urls, 0);
        if (cJSON_IsString(first)) return strdup(first->valuestring);
    }
    return NULL;
}

esp_err_t pipecat_signaling_fetch_ice_servers(pipecat_signaling_t *sig,
                                              pipecat_ice_server_t **out_servers,
                                              size_t *out_count)
{
    if (!sig || !out_servers || !out_count) return ESP_ERR_INVALID_ARG;
    *out_servers = NULL;
    *out_count   = 0;

    // Build base_url + "/ice-servers".
    size_t base_len = strlen(sig->base_url);
    while (base_len > 0 && sig->base_url[base_len - 1] == '/') base_len--;
    char *url = malloc(base_len + sizeof(ICE_PATH));
    if (!url) return ESP_ERR_NO_MEM;
    memcpy(url, sig->base_url, base_len);
    memcpy(url + base_len, ICE_PATH, sizeof(ICE_PATH));

    char *resp = malloc(MAX_ICE_RESPONSE);
    if (!resp) { free(url); return ESP_ERR_NO_MEM; }

    int status = 0;
    esp_err_t err = do_request(url, HTTP_METHOD_GET, NULL, resp,
                               MAX_ICE_RESPONSE, &status);
    free(url);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ice-servers GET failed: %s", esp_err_to_name(err));
        free(resp);
        return err;
    }
    if (status / 100 != 2) {
        ESP_LOGE(TAG, "ice-servers status=%d body=%s", status, resp);
        free(resp);
        return ESP_FAIL;
    }

    cJSON *root = cJSON_Parse(resp);
    free(resp);
    if (!root) {
        ESP_LOGE(TAG, "ice-servers response is not JSON");
        return ESP_FAIL;
    }
    const cJSON *arr = cJSON_GetObjectItem(root, "iceServers");
    if (!cJSON_IsArray(arr)) {
        ESP_LOGW(TAG, "ice-servers: 'iceServers' missing or not an array");
        cJSON_Delete(root);
        return ESP_OK;  // backend has no TURN configured — host-only ICE
    }

    int n = cJSON_GetArraySize(arr);
    if (n <= 0) {
        cJSON_Delete(root);
        ESP_LOGI(TAG, "ice-servers: empty list (no TURN configured)");
        return ESP_OK;
    }

    pipecat_ice_server_t *out = calloc(n, sizeof(*out));
    if (!out) { cJSON_Delete(root); return ESP_ERR_NO_MEM; }
    size_t kept = 0;
    for (int i = 0; i < n; ++i) {
        const cJSON *item = cJSON_GetArrayItem(arr, i);
        if (!cJSON_IsObject(item)) continue;
        char *u = extract_first_url(cJSON_GetObjectItem(item, "urls"));
        if (!u) continue;
        out[kept].url        = u;
        const cJSON *user = cJSON_GetObjectItem(item, "username");
        const cJSON *cred = cJSON_GetObjectItem(item, "credential");
        out[kept].username   = (cJSON_IsString(user) && user->valuestring[0])
                                ? strdup(user->valuestring) : NULL;
        out[kept].credential = (cJSON_IsString(cred) && cred->valuestring[0])
                                ? strdup(cred->valuestring) : NULL;
        ESP_LOGI(TAG, "ice-server[%u]: %s (user=%s)",
                 (unsigned)kept, out[kept].url,
                 out[kept].username ? out[kept].username : "<none>");
        ++kept;
    }
    cJSON_Delete(root);

    if (kept == 0) {
        free(out);
        return ESP_OK;
    }
    *out_servers = out;
    *out_count   = kept;
    return ESP_OK;
}
