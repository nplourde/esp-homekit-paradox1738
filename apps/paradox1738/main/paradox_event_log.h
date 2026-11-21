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

/* One event-log filter, exactly as the web API applies them: a class filter
 * compares event->cls, and a zone filter keeps only zone-class events whose
 * category is that zone (partition or fire reports carry a category that is
 * not a zone, so they never match). class_match and zone_match both false
 * means "keep everything". Keeping this here rather than open-coding it in the
 * request handler is what makes the filtering testable off-device. */
bool paradox_event_log_matches(const paradox_event_t *event,
                               bool class_match, paradox_class_t class_filter,
                               bool zone_match, unsigned zone_filter);

#endif
