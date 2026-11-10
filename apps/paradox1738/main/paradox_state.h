#ifndef PARADOX_STATE_H
#define PARADOX_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "paradox_proto.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool open;
    bool in_alarm;
    bool tampered;
    bool bypassed;
    uint64_t last_change_ms;
} paradox_zone_state_t;

typedef struct {
    paradox_zone_state_t zones[PARADOX_MAX_ZONES];
    bool fire_alarm;
    bool fire_restored;
    uint64_t fire_last_change_ms;
    uint64_t generation;
} paradox_state_snapshot_t;

typedef enum {
    PARADOX_STATE_CHANGE_ZONE = 1,
    PARADOX_STATE_CHANGE_FIRE,
} paradox_state_change_kind_t;

typedef void (*paradox_state_observer_t)(
    paradox_state_change_kind_t kind, int zone_id,
    const paradox_state_snapshot_t *snapshot, void *context);

typedef enum {
    PARADOX_STATE_OK = 0,
    PARADOX_STATE_NO_CHANGE,
    PARADOX_STATE_INVALID_ARGUMENT,
    PARADOX_STATE_ZONE_OUT_OF_RANGE,
    PARADOX_STATE_UNSUPPORTED_EVENT,
    PARADOX_STATE_NOT_INITIALIZED,
} paradox_state_result_t;

bool paradox_state_init(void);
paradox_state_result_t paradox_state_apply_event(
    const paradox_event_t *event, uint64_t timestamp_ms);
paradox_state_result_t paradox_state_snapshot(
    paradox_state_snapshot_t *snapshot);
paradox_state_result_t paradox_state_set_observer(
    paradox_state_observer_t observer, void *context);

#ifdef __cplusplus
}
#endif

#endif
