#ifndef PARADOX_EVENT_LOG_H
#define PARADOX_EVENT_LOG_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif

#include "paradox_proto.h"

#ifdef CONFIG_PARADOX_EVENT_LOG_CAPACITY
#define PARADOX_EVENT_LOG_CAPACITY CONFIG_PARADOX_EVENT_LOG_CAPACITY
#else
#define PARADOX_EVENT_LOG_CAPACITY 64u
#endif

typedef struct {
    paradox_event_t event;
    uint64_t timestamp_ms;
} paradox_event_log_entry_t;

void paradox_event_log_push(const paradox_event_t *event, uint64_t timestamp_ms);
size_t paradox_event_log_snapshot(paradox_event_log_entry_t *entries,
                                  size_t capacity);
void paradox_event_log_clear(void);

#endif
