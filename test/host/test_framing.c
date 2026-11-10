/*
 * T1.3 / T1.4 / T1.5: framing behaviour of the stream decoder.
 *
 * The panel stream has no header or checksum, so a single lost byte shifts the
 * phase permanently. These tests cover recovery from that, and prove that a
 * partial read never yields a phantom event.
 */
#include <stdlib.h>
#include <string.h>
#include <unity.h>

#include "unity_compat.h"

#include "paradox1738.h"

void setUp(void) {}
void tearDown(void) {}

/* The decoder also validates bytes 3-4 (the panel clock: HHHHH xHmm / mmmm
 * 0000), so the fixtures have to carry a real time. */
static void set_clock(uint8_t *frame, uint8_t hour, uint8_t minute)
{
    frame[2] = (uint8_t)((hour << 3) | (minute >> 4));
    frame[3] = (uint8_t)((minute & 0x0Fu) << 4);
}

static void make_frame(uint8_t *frame, uint8_t event_id, uint8_t category_id)
{
    frame[0] = (uint8_t)((event_id << 2) | ((category_id >> 4) & 0x03u));
    frame[1] = (uint8_t)((category_id & 0x0Fu) << 4);
    set_clock(frame, 20, 0);
}

/* Zone N open, the event used by the fixtures below. */
static void zone_open(uint8_t *frame, uint8_t zone)
{
    make_frame(frame, PARADOX_EVT_ZONE_OPEN, zone);
}

/* Everything one push produced: the events it wrote inline into the caller's
 * array, followed by any that had to be queued because the array was full. */
static size_t push_drain(paradox_decoder_t *dec, const uint8_t *bytes, size_t len,
                         paradox_event_t *evt, size_t max)
{
    size_t n = paradox_decoder_push(dec, bytes, len, evt, max);

    while (n < max && paradox_decoder_pop(dec, &evt[n])) {
        n++;
    }
    return n;
}

/* T1.3: a valid frame, then a stream with one byte deleted mid-way, then more
 * valid frames. Alignment must come back within one discarded frame. */
void test_resync_after_single_dropped_byte(void)
{
    paradox_decoder_t dec;
    paradox_event_t evt[8];
    size_t n;

    paradox_decoder_init(&dec);

    uint8_t f1[4], f2[4], f3[4];
    zone_open(f1, 1);
    zone_open(f2, 2);
    zone_open(f3, 3);

    /* Clean frame first. */
    TEST_ASSERT_EQUAL_SIZE(1, paradox_decoder_push(&dec, f1, 4, evt, 8));
    TEST_ASSERT_EQUAL_UINT8(2, evt[0].event_id);
    TEST_ASSERT_EQUAL_UINT8(1, evt[0].category_id);

    /* Now f2 with its second byte removed: the stream is misaligned from here. */
    uint8_t corrupt[3] = {f2[0], f2[2], f2[3]};
    uint8_t joined[7];
    memcpy(joined, corrupt, 3);
    memcpy(joined + 3, f3, 4);

    n = push_drain(&dec, joined, sizeof(joined), evt, 8);

    /* The event that survives must be a real one (f3), not a mash-up of the
     * two frames, and we must have counted exactly one framing error. */
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.framing_errors);
    TEST_ASSERT_EQUAL_SIZE(1, n);
    TEST_ASSERT_EQUAL_UINT8(2, evt[0].event_id);
    TEST_ASSERT_EQUAL_UINT8(3, evt[0].category_id);

    /* And the decoder is aligned again: a following frame decodes cleanly with
     * no further framing errors. */
    uint8_t f4[4];
    zone_open(f4, 4);
    n = push_drain(&dec, f4, sizeof(f4), evt, 8);
    TEST_ASSERT_EQUAL_SIZE(1, n);
    TEST_ASSERT_EQUAL_UINT8(4, evt[0].category_id);
    TEST_ASSERT_EQUAL_UINT32(1, dec.stats.framing_errors);
}

/* T1.3: exactly one framing error per injected corruption. */
void test_one_framing_error_per_corruption(void)
{
    paradox_decoder_t dec;
    paradox_event_t evt[8];
    size_t n = 0;

    paradox_decoder_init(&dec);

    for (int i = 0; i < 5; i++) {
        uint8_t good[4];
        zone_open(good, (uint8_t)(i + 1));
        /* Insert a byte that cannot be part of any frame (event id 0). */
        uint8_t junk[5] = {0x00, 0x00, 0x00, good[0], good[1]};
        n += push_drain(&dec, junk, sizeof(junk), evt + n, (sizeof(evt) / sizeof(evt[0])) - n);
        uint8_t tail[2] = {good[2], good[3]};
        n += push_drain(&dec, tail, sizeof(tail), evt + n, (sizeof(evt) / sizeof(evt[0])) - n);
    }

    TEST_ASSERT_EQUAL_UINT32(5, dec.stats.framing_errors);
    TEST_ASSERT_GREATER_THAN_UINT32(0, dec.stats.bytes_discarded);
    /* Every injected frame still made it through. */
    TEST_ASSERT_EQUAL_SIZE(5, n);
    for (size_t i = 0; i < n; i++) {
        TEST_ASSERT_EQUAL_UINT8(PARADOX_EVT_ZONE_OPEN, evt[i].event_id);
        TEST_ASSERT_EQUAL_UINT8((uint8_t)(i + 1), evt[i].category_id);
    }
}

/* T1.3: no hang and no event when fewer than four bytes are available. */
void test_partial_input_never_hangs_or_emits(void)
{
    /* One decoder per prefix length: reusing a single decoder would let the
     * last iteration complete the frame buffered by the earlier ones. */
    for (size_t len = 0; len < PARADOX_FRAME_LEN; len++) {
        uint8_t good[4];
        paradox_decoder_t dec;
        paradox_event_t evt[4];

        zone_open(good, 1);
        paradox_decoder_init(&dec);
        memset(evt, 0xA5, sizeof(evt));

        size_t produced = paradox_decoder_push(&dec, good, len, evt, 4);
        TEST_ASSERT_EQUAL_SIZE_MESSAGE(0, produced, "partial read must not produce an event");
        TEST_ASSERT_EQUAL_UINT32(0, paradox_decoder_pop(&dec, &evt[0]));
        TEST_ASSERT_EQUAL_UINT32(0, dec.stats.frames_ok);
        TEST_ASSERT_EQUAL_UINT32(0, dec.stats.framing_errors);
    }
}

/* T1.4: the caller struct is untouched when there is nothing to report. */
void test_caller_struct_untouched_on_no_event(void)
{
    paradox_decoder_t dec;
    paradox_event_t sentinel;
    uint8_t pattern[sizeof(sentinel)];

    paradox_decoder_init(&dec);
    memset(pattern, 0x5A, sizeof(pattern));
    memcpy(&sentinel, pattern, sizeof(sentinel));

    uint8_t partial[3] = {0x08, 0x10, 0x34};
    size_t produced = paradox_decoder_push(&dec, partial, sizeof(partial), &sentinel, 1);
    TEST_ASSERT_EQUAL_SIZE(0, produced);
    TEST_ASSERT_FALSE(paradox_decoder_pop(&dec, &sentinel));
    TEST_ASSERT_EQUAL_MEMORY(pattern, &sentinel, sizeof(sentinel));
}

/* T1.4: reading one byte at a time gives the same events as one big chunk, and
 * never reads past the frame boundary. */
void test_bytewise_and_chunked_agree(void)
{
    uint8_t stream[4 * 6];
    for (uint8_t z = 1; z <= 6; z++) {
        zone_open(stream + (z - 1) * 4, z);
    }

    paradox_decoder_t bytewise, chunked;
    paradox_event_t a[8], b[8];
    size_t na = 0, nb = 0;

    paradox_decoder_init(&bytewise);
    paradox_decoder_init(&chunked);

    for (size_t i = 0; i < sizeof(stream); i++) {
        na += push_drain(&bytewise, stream + i, 1, a + na, 8 - na);
    }
    nb = push_drain(&chunked, stream, sizeof(stream), b, 8);

    TEST_ASSERT_EQUAL_SIZE(6, na);
    TEST_ASSERT_EQUAL_SIZE(6, nb);
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof(paradox_event_t) * 6);
    TEST_ASSERT_EQUAL_UINT32(0, bytewise.stats.framing_errors);
    TEST_ASSERT_EQUAL_UINT32(0, chunked.stats.framing_errors);
}

/* Events the caller does not take are queued rather than lost, and stay in
 * order. */
void test_events_queue_when_caller_saturated(void)
{
    paradox_decoder_t dec;
    paradox_event_t evt[2];
    size_t n;

    paradox_decoder_init(&dec);

    uint8_t stream[4 * 5];
    for (uint8_t z = 1; z <= 5; z++) {
        zone_open(stream + (z - 1) * 4, z);
    }

    /* Only room for two of the five. */
    TEST_ASSERT_EQUAL_SIZE(2, paradox_decoder_push(&dec, stream, sizeof(stream), evt, 2));
    TEST_ASSERT_EQUAL_UINT8(1, evt[0].category_id);
    TEST_ASSERT_EQUAL_UINT8(2, evt[1].category_id);
    TEST_ASSERT_EQUAL_SIZE(3, paradox_decoder_pending(&dec));

    /* The three that did not fit come back in order. */
    n = 0;
    while (n < 2 && paradox_decoder_pop(&dec, &evt[n])) {
        n++;
    }
    TEST_ASSERT_EQUAL_SIZE(2, n);
    TEST_ASSERT_EQUAL_UINT8(3, evt[0].category_id);
    TEST_ASSERT_EQUAL_UINT8(4, evt[1].category_id);

    paradox_decoder_pop(&dec, &evt[0]);
    TEST_ASSERT_EQUAL_UINT8(5, evt[0].category_id);
    TEST_ASSERT_FALSE(paradox_decoder_pop(&dec, &evt[0]));
}

/* A long run of junk must terminate and eventually re-align. */
void test_realigns_out_of_long_junk_run(void)
{
    paradox_decoder_t dec;
    paradox_event_t evt[4];
    size_t n;

    paradox_decoder_init(&dec);

    uint8_t junk[37];
    memset(junk, 0xFF, sizeof(junk)); /* 0xFF decodes to event 63: impossible */
    paradox_decoder_push(&dec, junk, sizeof(junk), evt, 4);

    uint8_t good[4];
    zone_open(good, 9);
    n = push_drain(&dec, good, sizeof(good), evt, 4);

    TEST_ASSERT_EQUAL_SIZE(1, n);
    TEST_ASSERT_EQUAL_UINT8(9, evt[0].category_id);
    TEST_ASSERT_GREATER_THAN_UINT32(0, dec.stats.framing_errors);
}

/* Stats can be cleared without disturbing the buffered tail. */
void test_stats_clear_keeps_alignment(void)
{
    paradox_decoder_t dec;
    paradox_event_t evt[4];
    size_t n;

    paradox_decoder_init(&dec);
    uint8_t good[4];
    zone_open(good, 3);
    paradox_decoder_push(&dec, good, 2, evt, 4); /* half a frame */
    memset(&dec.stats, 0, sizeof(dec.stats));
    TEST_ASSERT_EQUAL_UINT32(0, dec.stats.frames_ok);

    n = push_drain(&dec, good + 2, 2, evt, 4);
    TEST_ASSERT_EQUAL_SIZE(1, n);
    TEST_ASSERT_EQUAL_UINT8(3, evt[0].category_id);
}

/* T1.5 (host half): the panel buffer is exactly 4 bytes and is never
 * NUL-terminated. Allocating it to the exact size means ASAN reports any read
 * past the frame, which is what an ESP_LOGI("%s", data) used to cause on the
 * device. Run this suite with SAN=1 for the check to mean anything. */
void test_non_terminated_frame_never_overread(void)
{
    paradox_decoder_t dec;
    paradox_event_t evt[4];

    paradox_decoder_init(&dec);

    uint8_t *frame = malloc(PARADOX_FRAME_LEN);
    TEST_ASSERT_NOT_NULL(frame);
    /* Deliberately no terminator and no padding byte. */
    frame[0] = (uint8_t)((PARADOX_EVT_ZONE_OPEN << 2) | ((3u >> 4) & 0x03u));
    frame[1] = (uint8_t)((3u & 0x0Fu) << 4);
    set_clock(frame, 20, 0);

    TEST_ASSERT_EQUAL_SIZE(1, paradox_decoder_push(&dec, frame, PARADOX_FRAME_LEN, evt, 4));
    TEST_ASSERT_EQUAL_UINT8(PARADOX_EVT_ZONE_OPEN, evt[0].event_id);
    TEST_ASSERT_EQUAL_UINT8(3, evt[0].category_id);

    /* A partial non-terminated read must be buffered, not scanned. */
    free(frame);
    frame = malloc(3);
    TEST_ASSERT_NOT_NULL(frame);
    memset(frame, 0x08, 3);
    TEST_ASSERT_EQUAL_SIZE(0, paradox_decoder_push(&dec, frame, 3, evt, 4));
    free(frame);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_resync_after_single_dropped_byte);
    RUN_TEST(test_one_framing_error_per_corruption);
    RUN_TEST(test_partial_input_never_hangs_or_emits);
    RUN_TEST(test_caller_struct_untouched_on_no_event);
    RUN_TEST(test_bytewise_and_chunked_agree);
    RUN_TEST(test_events_queue_when_caller_saturated);
    RUN_TEST(test_realigns_out_of_long_junk_run);
    RUN_TEST(test_stats_clear_keeps_alignment);
    RUN_TEST(test_non_terminated_frame_never_overread);
    return UNITY_END();
}
