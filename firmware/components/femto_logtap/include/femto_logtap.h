// Live log tap for the web UI's WebSocket log view (PRD §6.7). Lines are
// forwarded only while a viewer is connected; nothing is kept otherwise.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** Hook ESP_LOG output (call once at boot). Costs nothing until enabled. */
void femto_logtap_install(void);

/** Start/stop forwarding (enable while ≥ 1 WebSocket viewer is open). */
void femto_logtap_enable(bool on);

/** Forward an already formatted line (the Rust logger's output). */
void femto_logtap_push(const char *text, size_t len);

/** Next log chunk into `out` (blocks up to `timeout_ms`). Returns bytes. */
size_t femto_logtap_receive(char *out, size_t cap, uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
