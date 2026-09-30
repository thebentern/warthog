/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "cdc_out.h"

#include <stdio.h>
#include <string.h>

size_t cdc_out_write(struct cdc_out *o, const char *s)
{
    if (o == NULL || s == NULL || s[0] == '\0' || !o->connected() || !o->lock(true)) {
        return 0;
    }
    size_t left = strlen(s), done = 0;
    uint32_t idle_ms = 0;
    /* Held across the waits: other writers drop their line rather than land inside this one. */
    while (left > 0 && o->connected()) {
        size_t n = o->queue((const uint8_t *)s + done, left);
        if (n > 0) {
            done += n;
            left -= n;
            idle_ms = 0;
            o->stalled = false;
            continue;
        }
        if (o->stalled || idle_ms >= CDC_OUT_IDLE_MS) {
            o->stalled = true;
            break;
        }
        o->wait_ms(CDC_OUT_POLL_MS);
        idle_ms += CDC_OUT_POLL_MS;
    }
    o->unlock();
    return done;
}

size_t cdc_out_write_nowait(const struct cdc_out *o, const char *s)
{
    if (o == NULL || s == NULL || s[0] == '\0' || !o->connected() || !o->lock(false)) {
        return 0;
    }
    /* Only the host's reads change room while the lock is held, and they only add to it. */
    size_t n = strlen(s), done = 0;
    if (o->room() >= n) {
        done = o->queue((const uint8_t *)s, n);
    }
    o->unlock();
    return done;
}

size_t cdc_out_vprintf_nowait(const struct cdc_out *o, const char *fmt, va_list ap)
{
    if (o == NULL || fmt == NULL || !o->connected()) {
        return 0;
    }
    char line[CDC_OUT_LINE_MAX];
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    if (n <= 0) {
        return 0;
    }
    if ((size_t)n >= sizeof(line)) {
        line[sizeof(line) - 2] = '\n';
    }
    return cdc_out_write_nowait(o, line);
}
