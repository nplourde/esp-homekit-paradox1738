#include "paradox_state.h"

#include <string.h>

#ifdef ESP_PLATFORM
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t state_mutex;

static bool state_lock(void)
{
    return state_mutex != NULL && xSemaphoreTake(state_mutex, portMAX_DELAY) == pdTRUE;
}

static void state_unlock(void)
{
    xSemaphoreGive(state_mutex);
}
#else
#include <pthread.h>

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool state_lock(void)
{
    return pthread_mutex_lock(&state_mutex) == 0;
}

static void state_unlock(void)
{
    (void)pthread_mutex_unlock(&state_mutex);
}
#endif

static paradox_state_snapshot_t current_state;
static paradox_state_observer_t current_observer;
static void *current_observer_context;

bool paradox_state_init(void)
{
#ifdef ESP_PLATFORM
    if (state_mutex == NULL) {
        state_mutex = xSemaphoreCreateMutex();
        if (state_mutex == NULL) {
            return false;
        }
    }
#endif
    if (!state_lock()) {
        return false;
    }
    memset(&current_state, 0, sizeof(current_state));
    current_observer = NULL;
    current_observer_context = NULL;
    state_unlock();
    return true;
}

static bool set_zone_field(bool *field, bool value)
{
    if (*field == value) {
        return false;
    }
    *field = value;
    return true;
}

paradox_state_result_t paradox_state_apply_event(
    const paradox_event_t *event, uint64_t timestamp_ms)
{
    if (event == NULL) {
        return PARADOX_STATE_INVALID_ARGUMENT;
    }
    if (paradox_event_class(event->event_id) != event->cls) {
        return PARADOX_STATE_UNSUPPORTED_EVENT;
    }
    if (event->cls == PARADOX_CLS_ZONE &&
        (event->category_id < 1 ||
         event->category_id > PARADOX_MAX_ZONES)) {
        return PARADOX_STATE_ZONE_OUT_OF_RANGE;
    }

    paradox_state_change_kind_t kind;
    int zone_id = 0;
    bool changed = false;
    if (event->event_id == PARADOX_EVT_FIRE_ALARM ||
        event->event_id == PARADOX_EVT_FIRE_RESTORE ||
        (event->event_id == PARADOX_EVT_SPECIAL_ALARM &&
         event->category_id == 2)) {
        kind = PARADOX_STATE_CHANGE_FIRE;
    } else if (event->cls == PARADOX_CLS_ZONE) {
        kind = PARADOX_STATE_CHANGE_ZONE;
        zone_id = event->category_id;
    } else {
        return PARADOX_STATE_UNSUPPORTED_EVENT;
    }

    if (!state_lock()) {
        return PARADOX_STATE_NOT_INITIALIZED;
    }

    if (kind == PARADOX_STATE_CHANGE_FIRE) {
        bool alarm = event->event_id == PARADOX_EVT_FIRE_ALARM ||
                     event->event_id == PARADOX_EVT_SPECIAL_ALARM;
        bool restored = event->event_id == PARADOX_EVT_FIRE_RESTORE;
        changed |= set_zone_field(&current_state.fire_alarm, alarm);
        changed |= set_zone_field(&current_state.fire_restored, restored);
        if (changed) {
            current_state.fire_last_change_ms = timestamp_ms;
        }
    } else {
        paradox_zone_state_t *zone = &current_state.zones[zone_id - 1];
        switch (event->event_id) {
        case PARADOX_EVT_ZONE_OK:
            changed = set_zone_field(&zone->open, false);
            break;
        case PARADOX_EVT_ZONE_OPEN:
        case PARADOX_EVT_ZONE_DELAYED_BREACH:
            changed = set_zone_field(&zone->open, true);
            break;
        case PARADOX_EVT_ZONE_ALARM:
            changed = set_zone_field(&zone->in_alarm, true);
            break;
        case PARADOX_EVT_ZONE_ALARM_RESTORE:
            changed = set_zone_field(&zone->in_alarm, false);
            break;
        case PARADOX_EVT_ZONE_BYPASSED:
        case PARADOX_EVT_AUTO_ZONE_SHUTDOWN:
            changed = set_zone_field(&zone->bypassed, true);
            break;
        case PARADOX_EVT_ZONE_TAMPER:
            changed = set_zone_field(&zone->tampered, true);
            break;
        case PARADOX_EVT_ZONE_TAMPER_RESTORE:
            changed = set_zone_field(&zone->tampered, false);
            break;
        default:
            state_unlock();
            return PARADOX_STATE_UNSUPPORTED_EVENT;
        }
        if (changed) {
            zone->last_change_ms = timestamp_ms;
        }
    }

    if (!changed) {
        state_unlock();
        return PARADOX_STATE_NO_CHANGE;
    }

    current_state.generation++;
    paradox_state_snapshot_t snapshot = current_state;
    paradox_state_observer_t observer = current_observer;
    void *context = current_observer_context;
    state_unlock();

    if (observer != NULL) {
        observer(kind, zone_id, &snapshot, context);
    }
    return PARADOX_STATE_OK;
}

paradox_state_result_t paradox_state_snapshot(
    paradox_state_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return PARADOX_STATE_INVALID_ARGUMENT;
    }
    if (!state_lock()) {
        return PARADOX_STATE_NOT_INITIALIZED;
    }
    *snapshot = current_state;
    state_unlock();
    return PARADOX_STATE_OK;
}

paradox_state_result_t paradox_state_set_observer(
    paradox_state_observer_t observer, void *context)
{
    if (!state_lock()) {
        return PARADOX_STATE_NOT_INITIALIZED;
    }
    current_observer = observer;
    current_observer_context = context;
    state_unlock();
    return PARADOX_STATE_OK;
}
