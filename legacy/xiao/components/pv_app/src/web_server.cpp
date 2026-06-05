#include "app/web_server.hpp"

#include <cstdlib>
#include <cstring>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"

#include "hal/mdns.hpp"
#include "hal/nvs_kv.hpp"

namespace {

constexpr const char* kTag = "web";

// LED override hold per command — re-armed on each click; auto-resumes after.
constexpr int kHoldMs = 60'000;

// Hostname persistence. Mirrors main.cpp's NVS namespace; key is local here.
constexpr const char* kNvsNamespace   = "pipecat-cfg";
constexpr const char* kNvsKeyHostname = "hostname";

// Handlers are C function pointers → context via a file-local singleton (same
// pattern as hal::SoftApPortal). The server lives for the whole program.
struct Ctx {
    app::Session*  session  = nullptr;
    const uint8_t* html     = nullptr;
    std::size_t    html_len = 0;
    httpd_handle_t server   = nullptr;
};
Ctx g_ctx{};

// ---- Log capture → WebSocket --------------------------------------------
// esp_log vprintf hook copies every log line into a stream buffer (and chains
// to the original so the USB-JTAG console still works). A broadcaster task
// drains the buffer and pushes it to connected WS clients.

vprintf_like_t       s_orig_vprintf = nullptr;
StreamBufferHandle_t s_log_buf      = nullptr;

constexpr int      kMaxWsClients = 4;
int                s_ws_fds[kMaxWsClients] = { -1, -1, -1, -1 };
SemaphoreHandle_t  s_ws_mux = nullptr;

void ws_add_client(int fd)
{
    if (!s_ws_mux) return;
    xSemaphoreTake(s_ws_mux, portMAX_DELAY);
    for (int& f : s_ws_fds) { if (f == fd) { xSemaphoreGive(s_ws_mux); return; } }
    for (int& f : s_ws_fds) { if (f < 0) { f = fd; break; } }
    xSemaphoreGive(s_ws_mux);
}

void ws_remove_client(int fd)
{
    if (!s_ws_mux) return;
    xSemaphoreTake(s_ws_mux, portMAX_DELAY);
    for (int& f : s_ws_fds) { if (f == fd) f = -1; }
    xSemaphoreGive(s_ws_mux);
}

int log_vprintf(const char* fmt, va_list ap)
{
    // Capture a copy into the ring (non-blocking; drop if full), then hand the
    // original args to the default console sink. MUST NOT call ESP_LOG here.
    if (s_log_buf) {
        char line[256];
        va_list cp;
        va_copy(cp, ap);
        int n = vsnprintf(line, sizeof line, fmt, cp);
        va_end(cp);
        if (n > 0) {
            if (n > static_cast<int>(sizeof line)) n = sizeof line;
            xStreamBufferSend(s_log_buf, line, static_cast<size_t>(n), 0);
        }
    }
    return s_orig_vprintf ? s_orig_vprintf(fmt, ap) : 0;
}

// Sending must happen on the httpd task → queue_work with a heap-owned copy.
struct WsSend { int fd; size_t len; };  // payload bytes follow this header
void ws_send_work(void* arg)
{
    auto* w = static_cast<WsSend*>(arg);
    httpd_ws_frame_t f{};
    f.final   = true;
    f.type    = HTTPD_WS_TYPE_TEXT;
    f.payload = reinterpret_cast<uint8_t*>(w) + sizeof(WsSend);
    f.len     = w->len;
    if (httpd_ws_send_frame_async(g_ctx.server, w->fd, &f) != ESP_OK) {
        ws_remove_client(w->fd);
    }
    free(w);
}

void log_broadcast_task(void*)
{
    static char chunk[512];
    while (true) {
        size_t n = xStreamBufferReceive(s_log_buf, chunk, sizeof chunk, pdMS_TO_TICKS(250));
        if (n == 0) continue;
        if (!s_ws_mux) continue;
        xSemaphoreTake(s_ws_mux, portMAX_DELAY);
        for (int fd : s_ws_fds) {
            if (fd < 0) continue;
            auto* w = static_cast<WsSend*>(malloc(sizeof(WsSend) + n));
            if (!w) continue;
            w->fd = fd; w->len = n;
            std::memcpy(reinterpret_cast<uint8_t*>(w) + sizeof(WsSend), chunk, n);
            if (httpd_queue_work(g_ctx.server, ws_send_work, w) != ESP_OK) free(w);
        }
        xSemaphoreGive(s_ws_mux);
    }
}

// ---- helpers -------------------------------------------------------------

int clampi(int x, int lo, int hi) { return x < lo ? lo : (x > hi ? hi : x); }

int query_int(httpd_req_t* req, const char* key, int def)
{
    char q[160];
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK) return def;
    char v[16];
    if (httpd_query_key_value(q, key, v, sizeof v) != ESP_OK) return def;
    return atoi(v);
}

bool valid_hostname(const char* s)
{
    size_t n = strlen(s);
    if (n < 1 || n > 32) return false;
    for (size_t i = 0; i < n; ++i) {
        char c = s[i];
        const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-';
        if (!ok) return false;
        if (c == '-' && (i == 0 || i == n - 1)) return false;
    }
    return true;
}

// ---- HTTP handlers -------------------------------------------------------

esp_err_t handleRoot(httpd_req_t* req)
{
    if (!g_ctx.html) return ESP_FAIL;
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, reinterpret_cast<const char*>(g_ctx.html), g_ctx.html_len);
}

esp_err_t handleEffect(httpd_req_t* req)
{
    if (!g_ctx.session) return ESP_FAIL;
    const int e  = clampi(query_int(req, "e",  3),    0, 4);
    const int r  = clampi(query_int(req, "r",  0),    0, 255);
    const int g  = clampi(query_int(req, "g",  0),    0, 255);
    const int b  = clampi(query_int(req, "b",  0),    0, 255);
    const int br = clampi(query_int(req, "br", 0x60), 0, 255);
    const int sp = clampi(query_int(req, "sp", 0x40), 0, 255);
    g_ctx.session->ui().overrideEffect(
        static_cast<hal::Effect>(e),
        hal::Rgb{static_cast<uint8_t>(r), static_cast<uint8_t>(g), static_cast<uint8_t>(b)},
        static_cast<uint8_t>(br), static_cast<uint8_t>(sp), kHoldMs);
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t handleState(httpd_req_t* req)
{
    if (!g_ctx.session) return ESP_FAIL;
    const int s = clampi(query_int(req, "s", 0), 0, 10);
    g_ctx.session->ui().overrideState(static_cast<domain::LedState>(s), kHoldMs);
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t handleResume(httpd_req_t* req)
{
    if (!g_ctx.session) return ESP_FAIL;
    g_ctx.session->ui().resumeAuto();
    return httpd_resp_sendstr(req, "ok");
}

esp_err_t handleDiag(httpd_req_t* req)
{
    if (!g_ctx.session) return ESP_FAIL;
    std::string j = g_ctx.session->diagJson();
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, j.c_str(), j.size());
}

// GET /wake.wav — the mic audio that triggered the most recent wake fire, as a
// downloadable WAV. GET /wake.json — its decision metrics. Together they let us
// collect the actual audio behind a (false or real) fire for model retraining.
esp_err_t handleWakeWav(httpd_req_t* req)
{
    if (!g_ctx.session) return ESP_FAIL;
    std::string wav, meta; uint32_t seq;
    if (!g_ctx.session->getWakeSample(wav, meta, seq)) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "no wake sample captured yet");
    }
    httpd_resp_set_type(req, "audio/wav");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"wake.wav\"");
    return httpd_resp_send(req, wav.data(), wav.size());
}

esp_err_t handleWakeJson(httpd_req_t* req)
{
    if (!g_ctx.session) return ESP_FAIL;
    std::string wav, meta; uint32_t seq;
    httpd_resp_set_type(req, "application/json");
    if (!g_ctx.session->getWakeSample(wav, meta, seq))
        return httpd_resp_sendstr(req, "{\"error\":\"no wake sample captured yet\"}");
    return httpd_resp_send(req, meta.c_str(), meta.size());
}

// /hostname?name=foo — set the mDNS hostname live + persist to NVS.
esp_err_t handleHostname(httpd_req_t* req)
{
    char q[96], name[40] = {0};
    if (httpd_req_get_url_query_str(req, q, sizeof q) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof name) != ESP_OK ||
        !valid_hostname(name)) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req, "bad hostname (1-32 chars, a-z 0-9 -)");
    }
    hal::Mdns::setHostname(name);
    if (auto nvs = hal::NvsKv::open(kNvsNamespace)) {
        nvs->setStr(kNvsKeyHostname, name);
        nvs->commit();
    }
    ESP_LOGI(kTag, "hostname set to '%s.local'", name);
    return httpd_resp_sendstr(req, "ok");
}

// POST /ota — stream the uploaded firmware .bin straight into the inactive OTA
// slot, then reboot into it. Rollback (sdkconfig) guards against a bad image.
void ota_reboot_task(void*) { vTaskDelay(pdMS_TO_TICKS(1200)); esp_restart(); }

esp_err_t handleOta(httpd_req_t* req)
{
    const esp_partition_t* part = esp_ota_get_next_update_partition(nullptr);
    if (!part) { httpd_resp_set_status(req, "500 Internal Server Error");
                 return httpd_resp_sendstr(req, "no OTA partition"); }

    esp_ota_handle_t oh = 0;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &oh) != ESP_OK) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "esp_ota_begin failed");
    }

    // Spin the ring rainbow while flashing (long hold; we reboot before it
    // lapses). resumeAuto() restores the normal LED on any failure path.
    app::Ui& ui = g_ctx.session->ui();
    ui.overrideEffect(hal::Effect::Rainbow, hal::Rgb{0, 0, 0}, 0x80, 0x40, 120'000);

    char buf[1536];
    int remaining = req->content_len, total = 0;
    while (remaining > 0) {
        int want = remaining < static_cast<int>(sizeof buf) ? remaining : static_cast<int>(sizeof buf);
        int r = httpd_req_recv(req, buf, want);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0 || esp_ota_write(oh, buf, r) != ESP_OK) {
            esp_ota_abort(oh);
            ui.resumeAuto();
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "upload/write failed");
        }
        total += r; remaining -= r;
    }

    esp_err_t e = esp_ota_end(oh);
    if (e != ESP_OK) {
        ui.resumeAuto();
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_sendstr(req,
            e == ESP_ERR_OTA_VALIDATE_FAILED ? "invalid firmware image" : "esp_ota_end failed");
    }
    if (esp_ota_set_boot_partition(part) != ESP_OK) {
        ui.resumeAuto();
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "set_boot_partition failed");
    }

    ESP_LOGW(kTag, "OTA: wrote %d bytes to %s; rebooting into new firmware", total, part->label);
    httpd_resp_sendstr(req, "ok — rebooting into new firmware");
    xTaskCreate(ota_reboot_task, "ota_reboot", 2048, nullptr, 5, nullptr);
    return ESP_OK;
}

// WebSocket: on handshake register the client fd; we only push (incoming
// frames are drained and ignored).
esp_err_t ws_logs_handler(httpd_req_t* req)
{
    if (req->method == HTTP_GET) {
        ws_add_client(httpd_req_to_sockfd(req));
        return ESP_OK;
    }
    httpd_ws_frame_t f{};
    f.type = HTTPD_WS_TYPE_TEXT;
    httpd_ws_recv_frame(req, &f, 0);   // length probe; payload ignored
    return ESP_OK;
}

} // namespace

namespace app {

std::optional<WebServer> WebServer::start(Session& session, const uint8_t* html,
                                          std::size_t html_len, uint16_t port)
{
    // Log plumbing first, so we capture everything from start() onward.
    s_log_buf = xStreamBufferCreate(8 * 1024, 1);
    s_ws_mux  = xSemaphoreCreateMutex();
    if (!s_log_buf || !s_ws_mux) {
        ESP_LOGE(kTag, "log buffer/mutex alloc failed");
        return std::nullopt;
    }

    httpd_config_t cfg     = HTTPD_DEFAULT_CONFIG();
    cfg.server_port        = port;
    cfg.lru_purge_enable   = true;
    cfg.max_open_sockets   = 7;          // room for the WS log clients
    cfg.stack_size         = 8192;       // OTA streaming + esp_ota_write headroom
    cfg.recv_wait_timeout  = 15;         // tolerate slow firmware uploads
    httpd_handle_t h = nullptr;
    if (httpd_start(&h, &cfg) != ESP_OK) {
        ESP_LOGE(kTag, "httpd_start failed on port %u", (unsigned)port);
        return std::nullopt;
    }

    g_ctx = Ctx{&session, html, html_len, h};

    httpd_uri_t u_root  { "/",        HTTP_GET, handleRoot,     nullptr };
    httpd_uri_t u_eff   { "/effect",  HTTP_GET, handleEffect,   nullptr };
    httpd_uri_t u_state { "/state",   HTTP_GET, handleState,    nullptr };
    httpd_uri_t u_res   { "/resume",  HTTP_GET, handleResume,   nullptr };
    httpd_uri_t u_diag  { "/diag",    HTTP_GET, handleDiag,     nullptr };
    httpd_uri_t u_wwav  { "/wake.wav", HTTP_GET, handleWakeWav,  nullptr };
    httpd_uri_t u_wjson { "/wake.json",HTTP_GET, handleWakeJson, nullptr };
    httpd_uri_t u_host  { "/hostname",HTTP_GET, handleHostname, nullptr };
    httpd_uri_t u_ota   { "/ota",     HTTP_POST,handleOta,      nullptr };
    httpd_uri_t u_ws{};
    u_ws.uri = "/ws/logs"; u_ws.method = HTTP_GET; u_ws.handler = ws_logs_handler;
    u_ws.is_websocket = true;
    httpd_register_uri_handler(h, &u_root);
    httpd_register_uri_handler(h, &u_eff);
    httpd_register_uri_handler(h, &u_state);
    httpd_register_uri_handler(h, &u_res);
    httpd_register_uri_handler(h, &u_diag);
    httpd_register_uri_handler(h, &u_wwav);
    httpd_register_uri_handler(h, &u_wjson);
    httpd_register_uri_handler(h, &u_host);
    httpd_register_uri_handler(h, &u_ota);
    httpd_register_uri_handler(h, &u_ws);

    xTaskCreatePinnedToCore(log_broadcast_task, "ws_log", 4096, nullptr, 4, nullptr, 0);

    // Now mirror the log to the WS stream (after the buffer + task exist).
    s_orig_vprintf = esp_log_set_vprintf(log_vprintf);

    ESP_LOGI(kTag, "web console up on http://<host>:%u/  (logs at ws://<host>/ws/logs)",
             (unsigned)port);
    return WebServer{h};
}

} // namespace app
