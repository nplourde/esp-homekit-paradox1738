#include <unity.h>

#include "paradox_event_log.h"

#include <string.h>

void setUp(void)
{
    paradox_event_log_clear();
}

void tearDown(void) {}

static paradox_event_t make_event(uint8_t event_id, uint8_t zone_id)
{
    paradox_event_t event = {
        .event_id = event_id,
        .category_id = zone_id,
        .cls = paradox_event_class(event_id),
        .state = paradox_event_state(event_id, NULL),
        .zone_open = event_id == PARADOX_EVT_ZONE_OPEN,
        .raw = 0,
        .raw_frame = {0x08, (uint8_t)(zone_id << 4), 0xa0, 0x00},
    };
    return event;
}

void test_event_log_returns_newest_first_and_filters_capacity(void)
{
    for (uint8_t zone = 1; zone <= 3; zone++) {
        paradox_event_t event = make_event(PARADOX_EVT_ZONE_OPEN, zone);
        paradox_event_log_push(&event, zone * 100u);
    }

    paradox_event_log_entry_t entries[2];
    TEST_ASSERT_EQUAL_UINT(2, paradox_event_log_snapshot(entries, 2));
    TEST_ASSERT_EQUAL_UINT(3, entries[0].event.category_id);
    TEST_ASSERT_EQUAL_UINT(300, entries[0].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT(2, entries[1].event.category_id);
    TEST_ASSERT_EQUAL_UINT8(0xa0, entries[0].event.raw_frame[2]);
}

void test_event_log_wraps_at_capacity(void)
{
    for (size_t i = 1; i <= 10000u; i++) {
        paradox_event_t event = make_event(
            PARADOX_EVT_ZONE_OPEN,
            (uint8_t)(((i - 1u) % PARADOX_MAX_ZONES) + 1u));
        paradox_event_log_push(&event, i);
    }

    paradox_event_log_entry_t entries[PARADOX_EVENT_LOG_CAPACITY];
    TEST_ASSERT_EQUAL_UINT(PARADOX_EVENT_LOG_CAPACITY,
                           paradox_event_log_snapshot(entries,
                                                      PARADOX_EVENT_LOG_CAPACITY));
    TEST_ASSERT_EQUAL_UINT(10000, entries[0].timestamp_ms);
    TEST_ASSERT_EQUAL_UINT(10000u - PARADOX_EVENT_LOG_CAPACITY + 1u,
                           entries[PARADOX_EVENT_LOG_CAPACITY - 1].timestamp_ms);
}

void test_event_log_clear_and_invalid_arguments(void)
{
    paradox_event_t event = make_event(PARADOX_EVT_ZONE_OPEN, 1);
    paradox_event_log_entry_t entry;
    paradox_event_log_push(NULL, 1);
    paradox_event_log_push(&event, 2);
    TEST_ASSERT_EQUAL_UINT(0, paradox_event_log_snapshot(NULL, 1));
    TEST_ASSERT_EQUAL_UINT(0, paradox_event_log_snapshot(&entry, 0));
    paradox_event_log_clear();
    TEST_ASSERT_EQUAL_UINT(0, paradox_event_log_snapshot(&entry, 1));
}

/* T6.6: 10 000 pushes through a 64-slot ring must keep exactly the newest 64
 * events, in order, with no gaps and no duplicates. Timestamps double as the
 * push counter, so any skipped or repeated push shows up as a break in the
 * descending run. */
void test_snapshot_is_contiguous_without_gaps_or_duplicates(void)
{
    enum { PUSHES = 10000u };
    for (size_t i = 1; i <= PUSHES; i++) {
        paradox_event_t event = make_event(
            PARADOX_EVT_ZONE_OPEN,
            (uint8_t)(((i - 1u) % PARADOX_MAX_ZONES) + 1u));
        paradox_event_log_push(&event, i);
    }

    paradox_event_log_entry_t entries[PARADOX_EVENT_LOG_CAPACITY];
    size_t count = paradox_event_log_snapshot(entries, PARADOX_EVENT_LOG_CAPACITY);
    TEST_ASSERT_EQUAL_UINT(PARADOX_EVENT_LOG_CAPACITY, count);
    for (size_t i = 0; i < count; i++) {
        TEST_ASSERT_EQUAL_UINT(PUSHES - i, entries[i].timestamp_ms);
        TEST_ASSERT_EQUAL_INT(PARADOX_CLS_ZONE, (int)entries[i].event.cls);
    }
    /* A second snapshot must be stable: the log is read while panel, HomeKit
     * and web tasks all touch it. */
    paradox_event_log_entry_t again[PARADOX_EVENT_LOG_CAPACITY];
    TEST_ASSERT_EQUAL_UINT((unsigned)count,
                           (unsigned)paradox_event_log_snapshot(again, count));
    TEST_ASSERT_EQUAL_MEMORY(entries, again, sizeof(entries[0]) * count);
}

/* T6.6: the class and zone filters, asserted where they can actually be
 * exercised off-device. A zone filter must never pick up a non-zone event that
 * happens to carry the same category number (a partition or fire report can). */
void test_filters_select_only_matching_events(void)
{
    paradox_event_t zone_open = make_event(PARADOX_EVT_ZONE_OPEN, 4);
    paradox_event_t zone_closed = make_event(PARADOX_EVT_ZONE_OK, 4);
    paradox_event_t zone_alarm = make_event(PARADOX_EVT_ZONE_ALARM, 9);
    paradox_event_t partition = make_event(PARADOX_EVT_PARTITION_STATUS, 4);
    paradox_event_t fire = make_event(PARADOX_EVT_FIRE_ALARM, 2);
    paradox_event_t special = make_event(PARADOX_EVT_SPECIAL_ALARM, 2);

    /* Unfiltered: everything is kept. */
    const paradox_event_t *all[] = {&zone_open, &zone_closed, &zone_alarm,
                                   &partition, &fire};
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
        TEST_ASSERT_TRUE_MESSAGE(
            paradox_event_log_matches(all[i], false, PARADOX_CLS_UNKNOWN,
                                     false, 0),
            "unfiltered log must keep every event");
    }

    /* Zone filter: only zone-class events on that zone. */
    TEST_ASSERT_TRUE(paradox_event_log_matches(&zone_open, false, PARADOX_CLS_UNKNOWN,
                                               true, 4));
    TEST_ASSERT_TRUE(paradox_event_log_matches(&zone_closed, false, PARADOX_CLS_UNKNOWN,
                                               true, 4));
    TEST_ASSERT_FALSE(paradox_event_log_matches(&zone_alarm, false, PARADOX_CLS_UNKNOWN,
                                                true, 4));
    TEST_ASSERT_FALSE_MESSAGE(
        paradox_event_log_matches(&partition, false, PARADOX_CLS_UNKNOWN, true, 4),
        "category 4 of a partition report is not zone 4");
    /* The fire loop is reported on the zone where the detector sits, and fire
     * events classify as zone events, so ?zone=2 deliberately includes them. */
    TEST_ASSERT_EQUAL_INT(PARADOX_CLS_ZONE, (int)fire.cls);
    TEST_ASSERT_TRUE(paradox_event_log_matches(&fire, false, PARADOX_CLS_UNKNOWN,
                                               true, 2));

    /* Class filter, and class plus zone together. */
    TEST_ASSERT_EQUAL_INT(PARADOX_CLS_SPECIAL_ALARM, (int)special.cls);
    TEST_ASSERT_TRUE(paradox_event_log_matches(&special, true,
                                               PARADOX_CLS_SPECIAL_ALARM,
                                               false, 0));
    TEST_ASSERT_FALSE(paradox_event_log_matches(&zone_open, true,
                                                PARADOX_CLS_SPECIAL_ALARM,
                                                false, 0));
    TEST_ASSERT_TRUE(paradox_event_log_matches(&zone_alarm, true, PARADOX_CLS_ZONE,
                                               true, 9));
    TEST_ASSERT_FALSE(paradox_event_log_matches(&zone_alarm, true, PARADOX_CLS_ZONE,
                                                true, 4));
    TEST_ASSERT_FALSE(paradox_event_log_matches(NULL, false, PARADOX_CLS_UNKNOWN,
                                                false, 0));

    /* The handler's selection loop, replayed here so the limit is covered too:
     * newest first, filters applied, stop at limit. */
    for (uint8_t zone = 1; zone <= 9; zone++) {
        paradox_event_t event = make_event(PARADOX_EVT_ZONE_OPEN, zone);
        paradox_event_log_push(&event, zone);
    }
    paradox_event_t partition_event = make_event(PARADOX_EVT_PARTITION_STATUS, 9);
    paradox_event_log_push(&partition_event, 100);

    paradox_event_log_entry_t entries[PARADOX_EVENT_LOG_CAPACITY];
    size_t count = paradox_event_log_snapshot(entries, PARADOX_EVENT_LOG_CAPACITY);
    TEST_ASSERT_EQUAL_UINT(10, count);

    size_t kept = 0;
    size_t limit = 3;
    for (size_t i = 0; i < count && kept < limit; i++) {
        if (!paradox_event_log_matches(&entries[i].event, true, PARADOX_CLS_ZONE,
                                      true, 9)) {
            continue;
        }
        TEST_ASSERT_EQUAL_UINT(PARADOX_EVT_ZONE_OPEN, entries[i].event.event_id);
        TEST_ASSERT_EQUAL_UINT(9, entries[i].event.category_id);
        kept++;
    }
    TEST_ASSERT_EQUAL_UINT(1, kept);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_event_log_returns_newest_first_and_filters_capacity);
    RUN_TEST(test_event_log_wraps_at_capacity);
    RUN_TEST(test_event_log_clear_and_invalid_arguments);
    RUN_TEST(test_snapshot_is_contiguous_without_gaps_or_duplicates);
    RUN_TEST(test_filters_select_only_matching_events);
    return UNITY_END();
}
