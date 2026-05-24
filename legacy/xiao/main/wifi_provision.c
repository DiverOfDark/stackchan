#include "wifi_provision.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "wifi_prov";

#define NVS_NAMESPACE       "pipecat-cfg"
#define NVS_KEY_SSID        "wifi_ssid"
#define NVS_KEY_PSK         "wifi_psk"
#define NVS_KEY_BACKEND     "backend_url"

#define MAX_SSID_LEN        32
#define MAX_PSK_LEN         64
#define MAX_BACKEND_URL_LEN 128

#define WIFI_CONNECT_ATTEMPTS 5

static wifi_provision_state_t s_state         = WIFI_PROV_STATE_BOOT;
static char                   s_backend_url[MAX_BACKEND_URL_LEN] = {0};
static EventGroupHandle_t     s_wifi_events   = NULL;
static httpd_handle_t         s_http_server   = NULL;
static esp_netif_t           *s_sta_netif     = NULL;
static int                    s_connect_tries = 0;

#define WIFI_EVENT_BIT_CONNECTED BIT0
#define WIFI_EVENT_BIT_FAILED    BIT1

// ---------- NVS helpers ---------------------------------------------------

static esp_err_t nvs_load_str(nvs_handle_t h, const char *key, char *dst, size_t dst_len)
{
    size_t len = dst_len;
    esp_err_t err = nvs_get_str(h, key, dst, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        dst[0] = '\0';
    }
    return err;
}

static esp_err_t nvs_load_credentials(char *ssid, char *psk, char *backend)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &h);
    if (err != ESP_OK) return err;
    esp_err_t r1 = nvs_load_str(h, NVS_KEY_SSID,    ssid,    MAX_SSID_LEN);
    esp_err_t r2 = nvs_load_str(h, NVS_KEY_PSK,     psk,     MAX_PSK_LEN);
    esp_err_t r3 = nvs_load_str(h, NVS_KEY_BACKEND, backend, MAX_BACKEND_URL_LEN);
    nvs_close(h);
    if (r1 == ESP_OK && r2 == ESP_OK && r3 == ESP_OK && ssid[0] != '\0' && backend[0] != '\0') {
        return ESP_OK;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t nvs_save_credentials(const char *ssid, const char *psk, const char *backend)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) return err;
    // Wipe first so a partial save (e.g. SSID OK then backend fails) doesn't
    // strand stale credentials that look loadable but point at the wrong
    // network.
    nvs_erase_all(h);
    err = nvs_set_str(h, NVS_KEY_SSID, ssid);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_PSK, psk);
    if (err == ESP_OK) err = nvs_set_str(h, NVS_KEY_BACKEND, backend);
    if (err == ESP_OK) err = nvs_commit(h);
    nvs_close(h);
    return err;
}

// ---------- Wi-Fi station path -------------------------------------------

static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (++s_connect_tries < WIFI_CONNECT_ATTEMPTS) {
            ESP_LOGW(TAG, "disconnected, retry %d/%d", s_connect_tries, WIFI_CONNECT_ATTEMPTS);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "exceeded connect attempts; giving up");
            xEventGroupSetBits(s_wifi_events, WIFI_EVENT_BIT_FAILED);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "got ip " IPSTR, IP2STR(&evt->ip_info.ip));
        // Reset so a later transient disconnect (which fires
        // STA_DISCONNECTED → connect retry loop) doesn't carry over the
        // pre-association attempt count and immediately declare failure.
        s_connect_tries = 0;
        s_state = WIFI_PROV_STATE_CONNECTED;
        xEventGroupSetBits(s_wifi_events, WIFI_EVENT_BIT_CONNECTED);
    }
}

static esp_err_t wifi_sta_connect(const char *ssid, const char *psk)
{
    s_sta_netif = esp_netif_create_default_wifi_sta();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &wifi_event_handler, NULL));

    wifi_config_t cfg = {0};
    strlcpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid));
    strlcpy((char *)cfg.sta.password, psk,  sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN; // accept open networks too

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));

    s_state = WIFI_PROV_STATE_CONNECTING;
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits = xEventGroupWaitBits(
        s_wifi_events, WIFI_EVENT_BIT_CONNECTED | WIFI_EVENT_BIT_FAILED,
        pdFALSE, pdFALSE, pdMS_TO_TICKS(30000));

    if (bits & WIFI_EVENT_BIT_CONNECTED) return ESP_OK;
    s_state = WIFI_PROV_STATE_FAILED;
    return ESP_FAIL;
}

// ---------- SoftAP captive portal path -----------------------------------

extern const char  captive_portal_index_html[];
extern const size_t captive_portal_index_html_len;

// Minimal application/x-www-form-urlencoded value extractor.
// `key` is the field name (e.g. "ssid"). Writes URL-decoded value into `out`.
// Returns ESP_OK on success, ESP_ERR_NOT_FOUND if the key isn't in `body`.
static esp_err_t form_get(const char *body, size_t body_len,
                          const char *key, char *out, size_t out_len)
{
    const size_t key_len = strlen(key);
    const char *p = body;
    const char *end = body + body_len;
    while (p < end) {
        // Find "key="
        const char *amp = memchr(p, '&', end - p);
        const char *segment_end = amp ? amp : end;
        if ((size_t)(segment_end - p) > key_len &&
            memcmp(p, key, key_len) == 0 && p[key_len] == '=') {
            const char *v = p + key_len + 1;
            size_t v_len = segment_end - v;
            // URL-decode in place into `out`.
            size_t o = 0;
            for (size_t i = 0; i < v_len && o + 1 < out_len; ++i) {
                char c = v[i];
                if (c == '+') {
                    out[o++] = ' ';
                } else if (c == '%' && i + 2 < v_len) {
                    char hex[3] = { v[i + 1], v[i + 2], 0 };
                    out[o++] = (char)strtol(hex, NULL, 16);
                    i += 2;
                } else {
                    out[o++] = c;
                }
            }
            out[o] = '\0';
            return ESP_OK;
        }
        p = segment_end + 1;
    }
    return ESP_ERR_NOT_FOUND;
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, captive_portal_index_html,
                           captive_portal_index_html_len);
}

static esp_err_t save_post_handler(httpd_req_t *req)
{
    char body[512];
    int total = 0;
    while (total < (int)sizeof(body) - 1) {
        int got = httpd_req_recv(req, body + total, sizeof(body) - 1 - total);
        if (got <= 0) break;
        total += got;
    }
    body[total > 0 ? total : 0] = '\0';

    char ssid[MAX_SSID_LEN]       = {0};
    char psk[MAX_PSK_LEN]         = {0};
    char backend[MAX_BACKEND_URL_LEN] = {0};

    if (form_get(body, total, "ssid",    ssid,    sizeof(ssid))    != ESP_OK ||
        form_get(body, total, "backend", backend, sizeof(backend)) != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_sendstr(req, "missing ssid or backend");
        return ESP_OK;
    }
    form_get(body, total, "psk", psk, sizeof(psk));  // optional for open APs

    esp_err_t err = nvs_save_credentials(ssid, psk, backend);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs save failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_sendstr(req, "failed to save");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<html><body style='font-family:sans-serif;max-width:24rem;margin:2rem auto'>"
        "<h1>Saved</h1><p>Rebooting now\xE2\x80\xA6 the device will connect to your "
        "network and reach out to the configured backend.</p></body></html>");

    ESP_LOGI(TAG, "credentials saved; rebooting in 1s");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
    return ESP_OK;
}

static esp_err_t softap_start(void)
{
    esp_netif_create_default_wifi_ap();
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);

    wifi_config_t cfg = {
        .ap = {
            .channel       = 1,
            .max_connection = 4,
            .authmode      = WIFI_AUTH_OPEN,
        },
    };
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid),
             WIFI_PROVISION_AP_PREFIX "%02x%02x", mac[4], mac[5]);
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());

    httpd_config_t http_cfg = HTTPD_DEFAULT_CONFIG();
    http_cfg.server_port = WIFI_PROVISION_HTTP_PORT;
    ESP_ERROR_CHECK(httpd_start(&s_http_server, &http_cfg));

    httpd_uri_t root = { .uri = "/",     .method = HTTP_GET,  .handler = root_get_handler };
    httpd_uri_t save = { .uri = "/save", .method = HTTP_POST, .handler = save_post_handler };
    httpd_register_uri_handler(s_http_server, &root);
    httpd_register_uri_handler(s_http_server, &save);

    s_state = WIFI_PROV_STATE_PROVISIONING;
    ESP_LOGI(TAG, "SoftAP up: SSID=\"%s\" — join and open http://192.168.4.1/",
             cfg.ap.ssid);
    return ESP_OK;
}

// ---------- Public entrypoint --------------------------------------------

esp_err_t wifi_provision_start(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_events = xEventGroupCreate();

    char ssid[MAX_SSID_LEN]       = {0};
    char psk[MAX_PSK_LEN]         = {0};
    char backend[MAX_BACKEND_URL_LEN] = {0};

    if (nvs_load_credentials(ssid, psk, backend) == ESP_OK) {
        strlcpy(s_backend_url, backend, sizeof(s_backend_url));
        ESP_LOGI(TAG, "stored credentials found, connecting to \"%s\"", ssid);
        if (wifi_sta_connect(ssid, psk) == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "STA connect failed; falling back to provisioning");
        // Wipe so a wrong password doesn't trap us.
        nvs_handle_t h;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_key(h, NVS_KEY_SSID);
            nvs_erase_key(h, NVS_KEY_PSK);
            nvs_commit(h);
            nvs_close(h);
        }
        // Tear down the STA stack cleanly. Without these, the wifi handlers
        // stay bound to s_wifi_events and fire on SoftAP events too, and
        // the STA netif lingers as a phantom interface.
        esp_event_handler_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifi_event_handler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &wifi_event_handler);
        esp_wifi_stop();
        esp_wifi_deinit();
        if (s_sta_netif) {
            esp_netif_destroy_default_wifi(s_sta_netif);
            s_sta_netif = NULL;
        }
    } else {
        ESP_LOGI(TAG, "no saved credentials; entering provisioning mode");
    }

    return softap_start();
}

wifi_provision_state_t wifi_provision_state(void)
{
    return s_state;
}

const char *wifi_provision_backend_url(void)
{
    return s_backend_url[0] ? s_backend_url : NULL;
}
