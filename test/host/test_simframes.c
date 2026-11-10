/*
 * The recorded-panel simulator (CONFIG_PARADOX_SIMULATOR) replays raw frames
 * through the real decoder, so its table has to be decodable. A frame with an
 * implausible panel clock is silently dropped by the plausibility check, which
 * makes a device smoke run look dead; this suite fails instead.
 *
 * paradox_sim.c only includes paradox1738.h (no ESP-IDF headers), so it
 * compiles on the host with -DCONFIG_PARADOX_SIMULATOR (see Makefile).
 */
#include <string.h>

#include "paradox1738.h"
#include "unity.h"
#include "unity_compat.h"

#define SIM_MAX_FRAMES 256

void setUp(void) {}
void tearDown(void) {}

static const uint8_t (*frames)[4];
static size_t frame_count;

static void load_table(void)
{
    frames = paradox_sim_recorded_frames();
    frame_count = paradox_sim_recorded_frame_count();
    TEST_ASSERT_NOT_NULL(frames);
    TEST_ASSERT_TRUE(frame_count > 0);
    TEST_ASSERT_TRUE(frame_count <= SIM_MAX_FRAMES);
}

/* Every recorded frame must pass the decoder's plausibility check on its own. */
void test_every_recorded_frame_is_plausible(void)
{
    load_table();
    for (size_t i = 0; i < frame_count; i++) {
        if (!paradox_frame_plausible(frames[i])) {
            TEST_FAIL_MESSAGE("recorded frame rejected by the decoder");
        }
    }
}

/* Feeding the whole table as one chunk must yield exactly one event per frame,
 * with no resync activity at all. */
void test_recorded_stream_decodes_without_resync(void)
{
    paradox_decoder_t dec;
    paradox_event_t evt[SIM_MAX_FRAMES];
    size_t produced;

    load_table();
    paradox_decoder_init(&dec);

    produced = paradox_decoder_push(&dec, &frames[0][0], frame_count * PARADOX_FRAME_LEN,
                                    evt, SIM_MAX_FRAMES);
    TEST_ASSERT_EQUAL_SIZE(frame_count, produced);
    TEST_ASSERT_EQUAL_SIZE(0, paradox_decoder_pending(&dec));
    TEST_ASSERT_EQUAL_UINT32(frame_count, dec.stats.frames_ok);
    TEST_ASSERT_EQUAL_UINT32(0, dec.stats.framing_errors);
    TEST_ASSERT_EQUAL_UINT32(0, dec.stats.bytes_discarded);
    TEST_ASSERT_EQUAL_UINT32(0, dec.stats.overruns);
}

/* Byte-at-a-time replay (what the firmware does per tick) must agree with the
 * single-chunk result. */
void test_recorded_stream_bytewise_matches(void)
{
    paradox_decoder_t dec;
    paradox_event_t evt;
    size_t events = 0;

    load_table();
    paradox_decoder_init(&dec);

    for (size_t i = 0; i < frame_count * PARADOX_FRAME_LEN; i++) {
        const uint8_t b = (&frames[0][0])[i];
        events += paradox_decoder_push(&dec, &b, 1, &evt, 1);
        while (paradox_decoder_pop(&dec, &evt)) {
            events++;
        }
    }
    TEST_ASSERT_EQUAL_SIZE(frame_count, events);
    TEST_ASSERT_EQUAL_UINT32(0, dec.stats.framing_errors);
}

/* The table is meant to cover zones 1-16 open and closed. CategoryId 16 lives
 * in byte 1's low bits, so a typo there shows up as a missing zone. */
void test_recorded_stream_covers_all_zones(void)
{
    paradox_decoder_t dec;
    paradox_event_t evt[SIM_MAX_FRAMES];
    bool seen_open[PARADOX_MAX_ZONES + 1];
    bool seen_closed[PARADOX_MAX_ZONES + 1];
    size_t produced;

    load_table();
    memset(seen_open, 0, sizeof(seen_open));
    memset(seen_closed, 0, sizeof(seen_closed));

    paradox_decoder_init(&dec);
    produced = paradox_decoder_push(&dec, &frames[0][0],
                                    frame_count * PARADOX_FRAME_LEN, evt, SIM_MAX_FRAMES);
    TEST_ASSERT_EQUAL_SIZE(frame_count, produced);

    for (size_t i = 0; i < produced; i++) {
        if (evt[i].event_id != PARADOX_EVT_ZONE_OK &&
            evt[i].event_id != PARADOX_EVT_ZONE_OPEN) {
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE(evt[i].category_id >= 1 &&
                                 evt[i].category_id <= PARADOX_MAX_ZONES,
                                 "zone event outside 1..16");
        if (evt[i].zone_open) {
            seen_open[evt[i].category_id] = true;
        } else {
            seen_closed[evt[i].category_id] = true;
        }
    }

    for (size_t zone = 1; zone <= PARADOX_MAX_ZONES; zone++) {
        TEST_ASSERT_TRUE_MESSAGE(seen_open[zone], "no open event recorded for a zone");
        TEST_ASSERT_TRUE_MESSAGE(seen_closed[zone], "no closed event recorded for a zone");
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_every_recorded_frame_is_plausible);
    RUN_TEST(test_recorded_stream_decodes_without_resync);
    RUN_TEST(test_recorded_stream_bytewise_matches);
    RUN_TEST(test_recorded_stream_covers_all_zones);
    return UNITY_END();
}
