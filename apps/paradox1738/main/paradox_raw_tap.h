/*
 * Raw panel-UART tap.
 *
 * The event decoder resynchronises by discarding bytes, so the decoded-event
 * log can never show what actually arrived on the wire -- and the discarded
 * bytes are exactly where the framing/phase problem lives. This module keeps a
 * byte-for-byte copy of everything read from the panel UART in a ring buffer,
 * which the authenticated web debug endpoints expose as hex so the link can be
 * investigated over Wi-Fi instead of with a serial cable.
 *
 * Deliberately free of ESP-IDF dependencies (only the guarded sdkconfig.h) so
 * it can be unit tested on the host like the other pure modules.
 */
#ifndef PARADOX_RAW_TAP_H
#define PARADOX_RAW_TAP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#ifdef CONFIG_PARADOX_RAW_TAP_CAPACITY
#define PARADOX_RAW_TAP_CAPACITY CONFIG_PARADOX_RAW_TAP_CAPACITY
#else
#define PARADOX_RAW_TAP_CAPACITY 2048u
#endif

/* Absolute byte position in the tap stream. Every recorded byte has its own
 * sequence number, so a reader can tell a gap from a contiguous run even after
 * the ring has wrapped. */
typedef struct {
    uint64_t seq;      /* in/out: next sequence number the reader wants */
    uint64_t dropped;  /* out: bytes that aged out of the ring unread */
} paradox_raw_tap_cursor_t;

typedef struct {
    bool enabled;
    uint64_t capacity;        /* ring size in bytes */
    uint64_t bytes_recorded;  /* bytes accepted since the last clear */
    uint64_t bytes_lost;      /* overwritten before any reader reached them */
    uint64_t bytes_retained;  /* still readable right now */
} paradox_raw_tap_stats_t;

void paradox_raw_tap_set_enabled(bool enabled);
bool paradox_raw_tap_enabled(void);
void paradox_raw_tap_clear(void);

/* Called from the UART read path with each chunk as it comes off the wire.
 * A no-op while the tap is disabled. */
void paradox_raw_tap_write(const uint8_t *bytes, size_t len);

/* Move a cursor to the last `bytes` retained bytes (0 = live edge). */
void paradox_raw_tap_seek(paradox_raw_tap_cursor_t *cursor, uint64_t bytes);

/* Copy up to `capacity` bytes starting at cursor->seq, advancing it. Returns
 * the number copied; cursor->dropped accumulates anything already lost. */
size_t paradox_raw_tap_read(uint8_t *out, size_t capacity,
                            paradox_raw_tap_cursor_t *cursor);

void paradox_raw_tap_stats(paradox_raw_tap_stats_t *out_stats);

#endif /* PARADOX_RAW_TAP_H */
