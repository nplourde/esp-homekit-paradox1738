#include "paradox_raw_tap.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
static portMUX_TYPE raw_tap_mux = portMUX_INITIALIZER_UNLOCKED;
#define RAW_TAP_LOCK() portENTER_CRITICAL(&raw_tap_mux)
#define RAW_TAP_UNLOCK() portEXIT_CRITICAL(&raw_tap_mux)
#else
#include <pthread.h>
static pthread_mutex_t raw_tap_mutex = PTHREAD_MUTEX_INITIALIZER;
#define RAW_TAP_LOCK() ((void)pthread_mutex_lock(&raw_tap_mutex))
#define RAW_TAP_UNLOCK() ((void)pthread_mutex_unlock(&raw_tap_mutex))
#endif

/* Sequence numbers are absolute: the Nth recorded byte has sequence N, and
 * first_seq is the oldest sequence still held in the ring. */
static uint8_t ring[PARADOX_RAW_TAP_CAPACITY];
static bool enabled;
static uint64_t next_seq;
static uint64_t first_seq;

void paradox_raw_tap_set_enabled(bool enable)
{
    RAW_TAP_LOCK();
    enabled = enable;
    RAW_TAP_UNLOCK();
}

bool paradox_raw_tap_enabled(void)
{
    RAW_TAP_LOCK();
    bool value = enabled;
    RAW_TAP_UNLOCK();
    return value;
}

void paradox_raw_tap_clear(void)
{
    RAW_TAP_LOCK();
    memset(ring, 0, sizeof(ring));
    next_seq = 0;
    first_seq = 0;
    RAW_TAP_UNLOCK();
}

void paradox_raw_tap_write(const uint8_t *bytes, size_t len)
{
    if (bytes == NULL || len == 0) {
        return;
    }

    RAW_TAP_LOCK();
    if (!enabled) {
        RAW_TAP_UNLOCK();
        return;
    }

    /* The UART read buffer is always smaller than the ring, but keep the newest
     * CAPACITY bytes anyway if a chunk ever arrives larger than that. */
    size_t keep = len > sizeof(ring) ? sizeof(ring) : len;
    const uint8_t *keep_bytes = bytes + (len - keep);

    size_t tail = (size_t)((next_seq + len - keep) % sizeof(ring));
    size_t head_room = sizeof(ring) - tail;
    if (keep <= head_room) {
        memcpy(&ring[tail], keep_bytes, keep);
    } else {
        memcpy(&ring[tail], keep_bytes, head_room);
        memcpy(ring, keep_bytes + head_room, keep - head_room);
    }
    next_seq += len;

    /* Bytes that have aged out of the ring are no longer retrievable by any
     * reader, whatever each reader's own cursor has reached. */
    if (next_seq > sizeof(ring)) {
        first_seq = next_seq - sizeof(ring);
    }
    RAW_TAP_UNLOCK();
}

void paradox_raw_tap_seek(paradox_raw_tap_cursor_t *cursor, uint64_t bytes)
{
    if (cursor == NULL) {
        return;
    }
    RAW_TAP_LOCK();
    cursor->seq = bytes >= next_seq ? first_seq : next_seq - bytes;
    RAW_TAP_UNLOCK();
}

size_t paradox_raw_tap_read(uint8_t *out, size_t capacity,
                            paradox_raw_tap_cursor_t *cursor)
{
    if (out == NULL || capacity == 0 || cursor == NULL) {
        return 0;
    }

    RAW_TAP_LOCK();
    /* The reader fell behind the ring: account for the gap explicitly instead of
     * silently shifting its window, so a client can report the hole. */
    if (cursor->seq < first_seq) {
        cursor->dropped += first_seq - cursor->seq;
        cursor->seq = first_seq;
    }
    if (cursor->seq > next_seq) {
        cursor->seq = next_seq;
    }

    uint64_t available = next_seq - cursor->seq;
    size_t count = available < capacity ? (size_t)available : capacity;
    size_t offset = (size_t)(cursor->seq % sizeof(ring));
    size_t head_room = sizeof(ring) - offset;
    if (count <= head_room) {
        memcpy(out, &ring[offset], count);
    } else {
        memcpy(out, &ring[offset], head_room);
        memcpy(out + head_room, ring, count - head_room);
    }
    cursor->seq += count;
    RAW_TAP_UNLOCK();
    return count;
}

void paradox_raw_tap_stats(paradox_raw_tap_stats_t *out_stats)
{
    if (out_stats == NULL) {
        return;
    }
    RAW_TAP_LOCK();
    out_stats->enabled = enabled;
    out_stats->capacity = (uint64_t)sizeof(ring);
    out_stats->bytes_recorded = next_seq;
    out_stats->bytes_lost = first_seq;
    out_stats->bytes_retained = next_seq - first_seq;
    RAW_TAP_UNLOCK();
}
