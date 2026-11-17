#include <unity.h>

#include "paradox_raw_tap.h"

#include <stdlib.h>
#include <string.h>

void setUp(void)
{
    paradox_raw_tap_clear();
    paradox_raw_tap_set_enabled(true);
}

void tearDown(void)
{
    paradox_raw_tap_set_enabled(false);
}

static void fill_pattern(uint8_t *out, size_t len, uint8_t start)
{
    for (size_t i = 0; i < len; i++) {
        out[i] = (uint8_t)(start + i);
    }
}

/* The whole point of the tap is that it keeps bytes the decoder throws away, so
 * it must record verbatim and stay inert while panel debug is off. */
void test_raw_tap_disabled_records_nothing(void)
{
    uint8_t chunk[4] = {0x08, 0x10, 0xa0, 0x00};
    paradox_raw_tap_set_enabled(false);
    paradox_raw_tap_write(chunk, sizeof(chunk));

    paradox_raw_tap_stats_t stats;
    paradox_raw_tap_stats(&stats);
    TEST_ASSERT_FALSE(stats.enabled);
    TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_recorded);
    TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_retained);

    paradox_raw_tap_cursor_t cursor = {.seq = 0, .dropped = 0};
    uint8_t out[8];
    TEST_ASSERT_EQUAL_UINT(0, paradox_raw_tap_read(out, sizeof(out), &cursor));
}

void test_raw_tap_records_every_byte_in_order(void)
{
    uint8_t chunk[10];
    fill_pattern(chunk, sizeof(chunk), 0x30);
    for (int i = 0; i < 3; i++) {
        paradox_raw_tap_write(chunk, sizeof(chunk));
    }

    paradox_raw_tap_stats_t stats;
    paradox_raw_tap_stats(&stats);
    TEST_ASSERT_TRUE(stats.enabled);
    TEST_ASSERT_EQUAL_UINT64(30, stats.bytes_recorded);
    TEST_ASSERT_EQUAL_UINT64(30, stats.bytes_retained);
    TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_lost);

    uint8_t out[64];
    paradox_raw_tap_cursor_t cursor = {.seq = 0, .dropped = 0};
    TEST_ASSERT_EQUAL_UINT(30, paradox_raw_tap_read(out, sizeof(out), &cursor));
    for (size_t i = 0; i < 30u; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x30 + (i % 10u)), out[i]);
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.dropped);
    TEST_ASSERT_EQUAL_UINT64(30, cursor.seq);
    TEST_ASSERT_EQUAL_UINT(0, paradox_raw_tap_read(out, sizeof(out), &cursor));
}

/* Once the ring wraps, only the newest CAPACITY bytes survive, and a reader that
 * was following the stream must be told about the hole instead of silently
 * receiving a shifted window. */
void test_raw_tap_wrap_around_and_gap_reporting(void)
{
    const size_t capacity = PARADOX_RAW_TAP_CAPACITY;
    uint8_t *chunk = malloc(capacity);
    TEST_ASSERT_NOT_NULL(chunk);
    fill_pattern(chunk, capacity, 0x01);

    paradox_raw_tap_cursor_t cursor = {.seq = 0, .dropped = 0};
    uint8_t probe[4];

    /* First pass fills the ring exactly; nothing has aged out yet. */
    paradox_raw_tap_write(chunk, capacity);
    TEST_ASSERT_EQUAL_UINT(4, paradox_raw_tap_read(probe, sizeof(probe),
                                                   &cursor));
    TEST_ASSERT_EQUAL_UINT8(0x01, probe[0]);

    /* Second pass overwrites everything: the reader's cursor is now behind the
     * oldest retained byte. */
    fill_pattern(chunk, capacity, 0x80);
    paradox_raw_tap_write(chunk, capacity);

    paradox_raw_tap_stats_t stats;
    paradox_raw_tap_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(2u * capacity, stats.bytes_recorded);
    TEST_ASSERT_EQUAL_UINT64(capacity, stats.bytes_retained);
    TEST_ASSERT_EQUAL_UINT64(capacity, stats.bytes_lost);

    uint8_t *out = malloc(capacity);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_UINT(capacity, paradox_raw_tap_read(out, capacity, &cursor));
    TEST_ASSERT_EQUAL_UINT64(capacity - 4u, cursor.dropped);
    for (size_t i = 0; i < capacity; i++) {
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(0x80 + i), out[i]);
    }

    /* A fresh cursor seeking the whole ring reads it contiguously, wrapping in
     * the middle of the buffer. */
    paradox_raw_tap_cursor_t fresh = {.seq = 0, .dropped = 0};
    paradox_raw_tap_seek(&fresh, capacity);
    TEST_ASSERT_EQUAL_UINT64(capacity, fresh.seq);
    TEST_ASSERT_EQUAL_UINT(capacity, paradox_raw_tap_read(out, capacity, &fresh));
    TEST_ASSERT_EQUAL_UINT8(0x80, out[0]);
    TEST_ASSERT_EQUAL_UINT64(2u * capacity, fresh.seq);
    TEST_ASSERT_EQUAL_UINT64(0, fresh.dropped);

    free(chunk);
    free(out);
}

void test_raw_tap_seek_clamps_and_clear_resets(void)
{
    uint8_t chunk[16];
    fill_pattern(chunk, sizeof(chunk), 0x40);
    paradox_raw_tap_write(chunk, sizeof(chunk));

    /* Asking for more than is retained gives the whole ring rather than an
     * out-of-range cursor. */
    paradox_raw_tap_cursor_t cursor = {.seq = 0, .dropped = 0};
    paradox_raw_tap_seek(&cursor, UINT64_MAX);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.seq);

    /* Seeking zero positions at the live edge: nothing to read until more
     * bytes arrive. */
    paradox_raw_tap_seek(&cursor, 0);
    TEST_ASSERT_EQUAL_UINT64(16, cursor.seq);
    uint8_t out[8];
    TEST_ASSERT_EQUAL_UINT(0, paradox_raw_tap_read(out, sizeof(out), &cursor));

    /* Seeking the last 4 bytes lands on the tail of what was written. */
    paradox_raw_tap_seek(&cursor, 4);
    TEST_ASSERT_EQUAL_UINT64(12, cursor.seq);
    TEST_ASSERT_EQUAL_UINT(4, paradox_raw_tap_read(out, sizeof(out), &cursor));
    TEST_ASSERT_EQUAL_UINT8(0x4c, out[0]);

    paradox_raw_tap_clear();
    paradox_raw_tap_stats_t stats;
    paradox_raw_tap_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_recorded);
    TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_retained);
    TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_lost);
    TEST_ASSERT_EQUAL_UINT(PARADOX_RAW_TAP_CAPACITY, stats.capacity);
}

void test_raw_tap_ignores_empty_and_oversized_writes(void)
{
    paradox_raw_tap_write(NULL, 4);
    paradox_raw_tap_write((const uint8_t *)"ab", 0);

    paradox_raw_tap_stats_t stats;
    paradox_raw_tap_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.bytes_recorded);

    /* An oversized chunk keeps the newest CAPACITY bytes and never corrupts the
     * ring indices. */
    uint8_t *big = malloc(PARADOX_RAW_TAP_CAPACITY + 37u);
    TEST_ASSERT_NOT_NULL(big);
    memset(big, 0x5a, PARADOX_RAW_TAP_CAPACITY + 37u);
    paradox_raw_tap_write(big, PARADOX_RAW_TAP_CAPACITY + 37u);
    paradox_raw_tap_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(PARADOX_RAW_TAP_CAPACITY + 37u, stats.bytes_recorded);
    TEST_ASSERT_EQUAL_UINT64(PARADOX_RAW_TAP_CAPACITY, stats.bytes_retained);

    paradox_raw_tap_cursor_t cursor = {.seq = 0, .dropped = 0};
    paradox_raw_tap_seek(&cursor, PARADOX_RAW_TAP_CAPACITY);
    uint8_t *out = malloc(PARADOX_RAW_TAP_CAPACITY);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_EQUAL_UINT(PARADOX_RAW_TAP_CAPACITY,
                           paradox_raw_tap_read(out, PARADOX_RAW_TAP_CAPACITY,
                                                &cursor));
    TEST_ASSERT_EQUAL_UINT8(0x5a, out[0]);
    TEST_ASSERT_EQUAL_UINT8(0x5a, out[PARADOX_RAW_TAP_CAPACITY - 1u]);

    free(big);
    free(out);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_raw_tap_disabled_records_nothing);
    RUN_TEST(test_raw_tap_records_every_byte_in_order);
    RUN_TEST(test_raw_tap_wrap_around_and_gap_reporting);
    RUN_TEST(test_raw_tap_seek_clamps_and_clear_resets);
    RUN_TEST(test_raw_tap_ignores_empty_and_oversized_writes);
    return UNITY_END();
}
