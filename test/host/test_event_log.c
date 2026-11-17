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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_event_log_returns_newest_first_and_filters_capacity);
    RUN_TEST(test_event_log_wraps_at_capacity);
    RUN_TEST(test_event_log_clear_and_invalid_arguments);
    return UNITY_END();
}
