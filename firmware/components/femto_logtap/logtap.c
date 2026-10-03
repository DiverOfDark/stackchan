#include "femto_logtap.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"

static RingbufHandle_t s_rb;
static volatile bool s_on;
static vprintf_like_t s_orig;

static int hook(const char *fmt, va_list ap)
{
    if (s_on && s_rb) {
        char line[256];
        va_list copy;
        va_copy(copy, ap);
        int n = vsnprintf(line, sizeof line, fmt, copy);
        va_end(copy);
        if (n > 0) {
            if (n >= (int)sizeof line) n = sizeof line - 1;
            // Never block a logging task: drop the line if the viewer lags.
            xRingbufferSend(s_rb, line, n, 0);
        }
    }
    return s_orig ? s_orig(fmt, ap) : 0;
}

void femto_logtap_install(void)
{
    if (s_rb) return;
    // Byte buffer in PSRAM-friendly size; multi-writer safe.
    s_rb = xRingbufferCreate(8192, RINGBUF_TYPE_BYTEBUF);
    if (s_rb) s_orig = esp_log_set_vprintf(hook);
}

void femto_logtap_enable(bool on)
{
    s_on = on;
}

size_t femto_logtap_receive(char *out, size_t cap, uint32_t timeout_ms)
{
    if (!s_rb) return 0;
    size_t n = 0;
    void *p = xRingbufferReceiveUpTo(s_rb, &n, pdMS_TO_TICKS(timeout_ms), cap);
    if (!p) return 0;
    memcpy(out, p, n);
    vRingbufferReturnItem(s_rb, p);
    return n;
}

void femto_logtap_push(const char *text, size_t len)
{
    if (s_on && s_rb && len) xRingbufferSend(s_rb, text, len, 0);
}
