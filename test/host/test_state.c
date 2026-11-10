/*
 * M2: concurrent zone/fire state, input bounds and change notifications.
 */
#include <unity.h>

#include "paradox_state.h"

#include <pthread.h>
#include <stdatomic.h>

void setUp(void)
{
    TEST_ASSERT_TRUE(paradox_state_init());
}

void tearDown(void) {}

static paradox_event_t make_event(uint8_t event_id, uint8_t category_id)
{
    paradox_event_t event = {
        .event_id = event_id,
        .category_id = category_id,
        .cls = paradox_event_class(event_id),
        .state = PARADOX_STATE_IGNORE,
        .zone_open = false,
        .raw = 0,
    };
    return event;
}

void test_zone_index_bounds_and_unknown_class_are_rejected(void)
{
    const int invalid_zones[] = {0, 17, -1};
    for (size_t i = 0; i < sizeof(invalid_zones) / sizeof(invalid_zones[0]); i++) {
        paradox_event_t event = make_event(PARADOX_EVT_ZONE_OPEN, invalid_zones[i]);
        TEST_ASSERT_EQUAL_INT(PARADOX_STATE_ZONE_OUT_OF_RANGE,
                              paradox_state_apply_event(&event, 100));
    }
    paradox_event_t invalid_fire = make_event(PARADOX_EVT_FIRE_ALARM, 0);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_ZONE_OUT_OF_RANGE,
                          paradox_state_apply_event(&invalid_fire, 100));

    paradox_event_t event = make_event(PARADOX_EVT_ZONE_OPEN, 16);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_apply_event(&event, 100));

    event = make_event(PARADOX_EVT_ZONE_OPEN, 1);
    event.cls = PARADOX_CLS_UNKNOWN;
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_UNSUPPORTED_EVENT,
                          paradox_state_apply_event(&event, 200));

    paradox_state_snapshot_t snapshot;
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_snapshot(&snapshot));
    TEST_ASSERT_EQUAL_UINT64(1, snapshot.generation);
    TEST_ASSERT_TRUE(snapshot.zones[15].open);
    TEST_ASSERT_FALSE(snapshot.zones[0].open);
}

static unsigned notification_count;
static paradox_state_change_kind_t last_change_kind;
static int last_change_zone;

static void count_notifications(paradox_state_change_kind_t kind, int zone_id,
                               const paradox_state_snapshot_t *snapshot,
                               void *context)
{
    (void)snapshot;
    (void)context;
    notification_count++;
    last_change_kind = kind;
    last_change_zone = zone_id;
}

void test_notifications_are_emitted_only_on_state_changes(void)
{
    notification_count = 0;
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_set_observer(count_notifications, NULL));

    paradox_event_t open = make_event(PARADOX_EVT_ZONE_OPEN, 3);
    paradox_event_t closed = make_event(PARADOX_EVT_ZONE_OK, 3);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_apply_event(&open, 10));
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_NO_CHANGE,
                          paradox_state_apply_event(&open, 11));
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_apply_event(&closed, 12));
    TEST_ASSERT_EQUAL_UINT(2, notification_count);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_CHANGE_ZONE, last_change_kind);
    TEST_ASSERT_EQUAL_INT(3, last_change_zone);
}

void test_alarm_tamper_bypass_and_fire_fields(void)
{
    paradox_event_t event = make_event(PARADOX_EVT_ZONE_ALARM, 5);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_apply_event(&event, 10));
    event = make_event(PARADOX_EVT_ZONE_TAMPER, 5);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_apply_event(&event, 11));
    event = make_event(PARADOX_EVT_ZONE_BYPASSED, 5);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_apply_event(&event, 12));
    event = make_event(PARADOX_EVT_FIRE_ALARM, 2);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_apply_event(&event, 13));

    paradox_state_snapshot_t snapshot;
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.zones[4].in_alarm);
    TEST_ASSERT_TRUE(snapshot.zones[4].tampered);
    TEST_ASSERT_TRUE(snapshot.zones[4].bypassed);
    TEST_ASSERT_FALSE(snapshot.zones[4].open);
    TEST_ASSERT_TRUE(snapshot.fire_alarm);
    TEST_ASSERT_FALSE(snapshot.fire_restored);

    event = make_event(PARADOX_EVT_FIRE_RESTORE, 2);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_apply_event(&event, 14));
    event = make_event(PARADOX_EVT_SPECIAL_ALARM, 2);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_apply_event(&event, 15));
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OK,
                          paradox_state_snapshot(&snapshot));
    TEST_ASSERT_TRUE(snapshot.fire_alarm);
    TEST_ASSERT_FALSE(snapshot.fire_restored);
}

typedef struct {
    int zone_id;
    atomic_bool done;
    atomic_uint errors;
} worker_context_t;

static void *toggle_zone(void *arg)
{
    worker_context_t *context = arg;
    paradox_event_t open = make_event(PARADOX_EVT_ZONE_OPEN, context->zone_id);
    paradox_event_t closed = make_event(PARADOX_EVT_ZONE_OK, context->zone_id);
    for (unsigned i = 0; i < 100000; i++) {
        if (paradox_state_apply_event(&open, 11) != PARADOX_STATE_OK ||
            paradox_state_apply_event(&closed, 22) != PARADOX_STATE_OK) {
            atomic_fetch_add(&context->errors, 1);
        }
    }
    atomic_store(&context->done, true);
    return NULL;
}

static void *read_snapshots(void *arg)
{
    worker_context_t *writers = arg;
    uint64_t previous_generation = 0;
    do {
        paradox_state_snapshot_t snapshot;
        if (paradox_state_snapshot(&snapshot) != PARADOX_STATE_OK) {
            atomic_fetch_add(&writers[0].errors, 1);
            continue;
        }
        if (snapshot.generation < previous_generation) {
            atomic_fetch_add(&writers[0].errors, 1);
        }
        previous_generation = snapshot.generation;

        const int zones[] = {3, 4};
        for (size_t i = 0; i < sizeof(zones) / sizeof(zones[0]); i++) {
            const paradox_zone_state_t *zone = &snapshot.zones[zones[i] - 1];
            if ((zone->open && zone->last_change_ms != 11) ||
                (!zone->open && zone->last_change_ms != 0 &&
                 zone->last_change_ms != 22)) {
                atomic_fetch_add(&writers[0].errors, 1);
            }
        }
    } while (!atomic_load(&writers[0].done) ||
             !atomic_load(&writers[1].done));
    return NULL;
}

void test_state_snapshots_are_consistent_under_concurrent_access(void)
{
    worker_context_t writers[2] = {
        {.zone_id = 3},
        {.zone_id = 4},
    };
    atomic_init(&writers[0].done, false);
    atomic_init(&writers[1].done, false);
    atomic_init(&writers[0].errors, 0);
    atomic_init(&writers[1].errors, 0);

    pthread_t writer_threads[2];
    pthread_t reader_threads[2];
    TEST_ASSERT_EQUAL_INT(0, pthread_create(
        &writer_threads[0], NULL, toggle_zone, &writers[0]));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(
        &writer_threads[1], NULL, toggle_zone, &writers[1]));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(
        &reader_threads[0], NULL, read_snapshots, writers));
    TEST_ASSERT_EQUAL_INT(0, pthread_create(
        &reader_threads[1], NULL, read_snapshots, writers));

    for (size_t i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(writer_threads[i], NULL));
    }
    for (size_t i = 0; i < 2; i++) {
        TEST_ASSERT_EQUAL_INT(0, pthread_join(reader_threads[i], NULL));
    }
    TEST_ASSERT_EQUAL_UINT(0, atomic_load(&writers[0].errors));
    TEST_ASSERT_EQUAL_UINT(0, atomic_load(&writers[1].errors));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_zone_index_bounds_and_unknown_class_are_rejected);
    RUN_TEST(test_notifications_are_emitted_only_on_state_changes);
    RUN_TEST(test_alarm_tamper_bypass_and_fire_fields);
    RUN_TEST(test_state_snapshots_are_consistent_under_concurrent_access);
    return UNITY_END();
}
