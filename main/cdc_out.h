/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WARTHOG_CDC_OUT_H
#define WARTHOG_CDC_OUT_H

/* AT output into the CDC TX FIFO. Freestanding so the host tests build it:
 * no SDK, no ESP-IDF, libc only. */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CDC_OUT_POLL_MS 10u  /* wait between attempts while the FIFO is full */
#define CDC_OUT_IDLE_MS 500u /* give up after this long with nothing taken */
#define CDC_OUT_LINE_MAX 256u /* a formatted no-wait line, NUL included */

struct cdc_out {
    /* Queue what fits of @p n bytes (the FIFO keeps no more); returns bytes taken. */
    size_t (*queue)(const uint8_t *p, size_t n);
    /* Bytes the FIFO would take now. */
    size_t (*room)(void);
    bool (*connected)(void);
    void (*wait_ms)(uint32_t ms);
    /* One writer at a time on the port, so no text lands inside another's.
     * @p wait false: one try, never blocks. true: may block, bounded. */
    bool (*lock)(bool wait);
    void (*unlock)(void);
    /* The last cdc_out_write gave up: the next one does not wait until the FIFO
     * takes a byte again. Owned by the one task that calls cdc_out_write. */
    bool stalled;
};

/* Queue all of @p s under the lock, waiting CDC_OUT_POLL_MS whenever the FIFO is
 * full. Gives up after CDC_OUT_IDLE_MS with nothing taken, at once while stalled,
 * or when the host disconnects. Blocks: only for a task that may wait. @returns bytes queued. */
size_t cdc_out_write(struct cdc_out *o, const char *s);

/* All of @p s or nothing, never waits, never touches stalled: safe from any task.
 * Dropped whole while another writer holds the lock or the FIFO lacks room. @returns bytes queued. */
size_t cdc_out_write_nowait(const struct cdc_out *o, const char *s);

/* cdc_out_write_nowait of the formatted text; one cut past CDC_OUT_LINE_MAX still ends in '\n'. */
size_t cdc_out_vprintf_nowait(const struct cdc_out *o, const char *fmt, va_list ap);

#endif
