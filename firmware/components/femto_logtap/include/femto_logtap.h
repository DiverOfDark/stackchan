// Log tap: copies every log line into a ring that one reader (web.rs's
// logs-ws thread) drains, feeding the web UI's live view and the upload to
// the backend (logship.rs).
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Hook ESP_LOG output (call once at boot). Costs nothing until enabled. */
void femto_logtap_install(void);

/** Start/stop collecting (on from boot). */
void femto_logtap_enable(bool on);

/** Forward an already formatted line (the Rust logger's output). */
void femto_logtap_push(const char *text, size_t len);

/** Next log chunk into `out` (blocks up to `timeout_ms`). Returns bytes. */
size_t femto_logtap_receive(char *out, size_t cap, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
