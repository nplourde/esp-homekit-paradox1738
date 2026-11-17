#include "paradox_event_log.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
static portMUX_TYPE event_log_mux = portMUX_INITIALIZER_UNLOCKED;
#define EVENT_LOG_LOCK() portENTER_CRITICAL(&event_log_mux)
#define EVENT_LOG_UNLOCK() portEXIT_CRITICAL(&event_log_mux)
#else
#include <pthread.h>
static pthread_mutex_t event_log_mutex = PTHREAD_MUTEX_INITIALIZER;
#define EVENT_LOG_LOCK() ((void)pthread_mutex_lock(&event_log_mutex))
#define EVENT_LOG_UNLOCK() ((void)pthread_mutex_unlock(&event_log_mutex))
#endif

static paradox_event_log_entry_t entries[PARADOX_EVENT_LOG_CAPACITY];
static size_t next_entry;
static size_t entry_count;

void paradox_event_log_push(const paradox_event_t *event, uint64_t timestamp_ms)
{
    if (event == NULL) {
        return;
    }

    EVENT_LOG_LOCK();
    entries[next_entry].event = *event;
    entries[next_entry].timestamp_ms = timestamp_ms;
    next_entry = (next_entry + 1u) % PARADOX_EVENT_LOG_CAPACITY;
    if (entry_count < PARADOX_EVENT_LOG_CAPACITY) {
        entry_count++;
    }
    EVENT_LOG_UNLOCK();
}

size_t paradox_event_log_snapshot(paradox_event_log_entry_t *out,
                                  size_t capacity)
{
    if (out == NULL || capacity == 0) {
        return 0;
    }

    EVENT_LOG_LOCK();
    size_t count = entry_count < capacity ? entry_count : capacity;
    size_t start = (next_entry + PARADOX_EVENT_LOG_CAPACITY - 1u) %
                   PARADOX_EVENT_LOG_CAPACITY;
    for (size_t i = 0; i < count; i++) {
        size_t index = (start + PARADOX_EVENT_LOG_CAPACITY - i) %
                       PARADOX_EVENT_LOG_CAPACITY;
        out[i] = entries[index];
    }
    EVENT_LOG_UNLOCK();
    return count;
}

void paradox_event_log_clear(void)
{
    EVENT_LOG_LOCK();
    memset(entries, 0, sizeof(entries));
    next_entry = 0;
    entry_count = 0;
    EVENT_LOG_UNLOCK();
}
