/*
 * T1.1 / T1.2: golden decode table and bit math for the Paradox 1738 protocol.
 */
#include <unity.h>

#include "unity_compat.h"

#include "paradox1738.h"

void setUp(void) {}
void tearDown(void) {}

/* Encodes a panel clock time into bytes 3-4 (HHHHH xHmm / mmmm 0000). The
 * decoder validates these too, so the fixtures have to carry a real time. */
static void set_clock(uint8_t *frame, uint8_t hour, uint8_t minute)
{
    frame[2] = (uint8_t)((hour << 3) | (minute >> 4));
    frame[3] = (uint8_t)((minute & 0x0Fu) << 4);
}

/* Builds a frame from an event/category pair using the inverse of the
 * documented bit layout, so the fixtures cannot drift from the code under test
 * in the same direction. */
static void make_frame(uint8_t *frame, uint8_t event_id, uint8_t category_id)
{
    frame[0] = (uint8_t)((event_id << 2) | ((category_id >> 4) & 0x03u));
    frame[1] = (uint8_t)((category_id & 0x0Fu) << 4);
    set_clock(frame, 20, 0);
}

static void decode_one(uint8_t event_id, uint8_t category_id, paradox_event_t *out)
{
    uint8_t frame[PARADOX_FRAME_LEN];
    paradox_decoder_t dec;

    make_frame(frame, event_id, category_id);
    paradox_decoder_init(&dec);
    TEST_ASSERT_EQUAL_SIZE(1, paradox_decoder_push(&dec, frame, sizeof(frame), out, 1));
}

/* Every EventId the panel can emit, with a representative CategoryId that is
 * valid for that event. */
typedef struct {
    uint8_t event_id;
    uint8_t category_id;
    paradox_class_t cls;
} golden_t;

static const golden_t goldens[] = {
    {1, 1, PARADOX_CLS_ZONE},
    {2, 16, PARADOX_CLS_ZONE},
    {3, 0, PARADOX_CLS_PARTITION_STATUS},
    {3, 11, PARADOX_CLS_PARTITION_STATUS},
    {5, 0, PARADOX_CLS_NON_REPORT},
    {5, 7, PARADOX_CLS_NON_REPORT},
    {6, 1, PARADOX_CLS_REMOTE},
    {7, 8, PARADOX_CLS_REMOTE},
    {8, 4, PARADOX_CLS_REMOTE},
    {9, 8, PARADOX_CLS_REMOTE},
    {10, 1, PARADOX_CLS_ACCESS_CODE},
    {11, 48, PARADOX_CLS_ACCESS_CODE},
    {12, 7, PARADOX_CLS_ZONE},
    {13, 48, PARADOX_CLS_ACCESS_CODE},
    {14, 0, PARADOX_CLS_SPECIAL_ARM},
    {14, 7, PARADOX_CLS_SPECIAL_ARM},
    {15, 2, PARADOX_CLS_ACCESS_CODE},
    {16, 48, PARADOX_CLS_ACCESS_CODE},
    {17, 48, PARADOX_CLS_ACCESS_CODE},
    {18, 0, PARADOX_CLS_SPECIAL_DISARM},
    {18, 3, PARADOX_CLS_SPECIAL_DISARM},
    {19, 5, PARADOX_CLS_ZONE},
    {20, 16, PARADOX_CLS_ZONE},
    {21, 1, PARADOX_CLS_ZONE},
    {22, 3, PARADOX_CLS_ZONE},
    {23, 16, PARADOX_CLS_ZONE},
    {24, 0, PARADOX_CLS_SPECIAL_ALARM},
    {24, 6, PARADOX_CLS_SPECIAL_ALARM},
    {25, 16, PARADOX_CLS_ZONE},
    {26, 1, PARADOX_CLS_ZONE},
    {27, 16, PARADOX_CLS_ZONE},
    {28, 1, PARADOX_CLS_TROUBLE},
    {28, 11, PARADOX_CLS_TROUBLE},
    {29, 7, PARADOX_CLS_TROUBLE},
    {30, 0, PARADOX_CLS_SPECIAL_REPORT},
    {30, 5, PARADOX_CLS_SPECIAL_REPORT},
};

void test_all_golden_vectors_decode(void)
{
    for (size_t i = 0; i < sizeof(goldens) / sizeof(goldens[0]); i++) {
        paradox_event_t evt;
        decode_one(goldens[i].event_id, goldens[i].category_id, &evt);

        TEST_ASSERT_EQUAL_UINT8_MESSAGE(goldens[i].event_id, evt.event_id,
                                        paradox_event_name(goldens[i].event_id));
        TEST_ASSERT_EQUAL_UINT8_MESSAGE(goldens[i].category_id, evt.category_id,
                                        paradox_event_name(goldens[i].event_id));
        TEST_ASSERT_EQUAL_MESSAGE(goldens[i].cls, (int)evt.cls,
                                  paradox_event_name(goldens[i].event_id));
    }
}

/* Events 0 and 4 are never sent: they must be rejected as unknown so the
 * framing decoder can use them to detect misalignment. */
void test_never_used_event_ids_are_unknown(void)
{
    TEST_ASSERT_EQUAL_INT(PARADOX_CLS_UNKNOWN, (int)paradox_event_class(0));
    TEST_ASSERT_EQUAL_INT(PARADOX_CLS_UNKNOWN, (int)paradox_event_class(4));
    TEST_ASSERT_EQUAL_INT(PARADOX_CLS_UNKNOWN, (int)paradox_event_class(31));
}

/* The mapping the HomeKit layer depends on. */
void test_zone_and_fire_semantics(void)
{
    bool open;

    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_CLOSED, (int)paradox_event_state(1, &open));
    TEST_ASSERT_FALSE(open);

    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OPEN, (int)paradox_event_state(2, &open));
    TEST_ASSERT_TRUE(open);

    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_FIRE, (int)paradox_event_state(21, &open));
    TEST_ASSERT_TRUE(open);

    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_FIRE_RESTORE, (int)paradox_event_state(23, &open));
    TEST_ASSERT_FALSE(open);

    /* Zone in alarm behaves like an open contact; restore closes it again. */
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_OPEN, (int)paradox_event_state(20, &open));
    TEST_ASSERT_TRUE(open);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_CLOSED, (int)paradox_event_state(22, &open));
    TEST_ASSERT_FALSE(open);

    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_TAMPER, (int)paradox_event_state(26, &open));
    TEST_ASSERT_TRUE(open);
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_ARMED, (int)paradox_event_state(13, &open));
    TEST_ASSERT_EQUAL_INT(PARADOX_STATE_DISARMED, (int)paradox_event_state(15, &open));
}

/* T1.2: the exact expressions from the reference implementation. */
void test_bit_math_matches_reference(void)
{
    /* Zone 1 closed: evt 1 -> 0x04, cat 1 -> low bits 0 + nibble 1. */
    TEST_ASSERT_EQUAL_UINT8(1, paradox_frame_event_id(0x04));
    TEST_ASSERT_EQUAL_UINT8(1, paradox_frame_category_id(0x04, 0x10));

    /* Zone 16 = 0b010000: the two low bits of byte0 carry the high bits of
     * CategoryId (here 0b01), the high nibble of byte1 the low four (here 0). */
    TEST_ASSERT_EQUAL_UINT8(1, paradox_frame_event_id(0x05));
    TEST_ASSERT_EQUAL_UINT8(16, paradox_frame_category_id(0x05, 0x00));
    TEST_ASSERT_EQUAL_UINT8(2, paradox_frame_event_id(0x09));
    TEST_ASSERT_EQUAL_UINT8(16, paradox_frame_category_id(0x09, 0x00));

    /* Boundary: data[0] low bits 0b11 and data[1] high nibble 0xF combine to
     * CategoryId 0x3F = 63, the widest value the 6-bit field can hold. */
    TEST_ASSERT_EQUAL_UINT8(63, paradox_frame_category_id(0x03, 0xF0));
    TEST_ASSERT_EQUAL_UINT8(0, paradox_frame_category_id(0x00, 0x00));

    /* Zone 15 uses the other extreme of byte1 (0xF0). */
    TEST_ASSERT_EQUAL_UINT8(15, paradox_frame_category_id(0x04, 0xF0));

    /* Fire alarm on zone 1, as captured from the panel. */
    TEST_ASSERT_EQUAL_UINT8(21, paradox_frame_event_id(0x54));
    TEST_ASSERT_EQUAL_UINT8(1, paradox_frame_category_id(0x54, 0x10));
}

/* The low nibble of byte1 carries no information and must not leak into either
 * decoded value. The two low bits of byte0 do carry information (the high bits
 * of CategoryId), so they only affect the category, never the event. */
void test_low_nibble_of_byte1_ignored(void)
{
    for (uint8_t nib = 0; nib < 16; nib++) {
        TEST_ASSERT_EQUAL_UINT8(21, paradox_frame_event_id(0x54));
        TEST_ASSERT_EQUAL_UINT8(1, paradox_frame_category_id(0x54, (uint8_t)(0x10 | nib)));
    }

    for (uint8_t low = 0; low < 4; low++) {
        /* EventId comes from the top six bits of byte0 only. */
        TEST_ASSERT_EQUAL_UINT8(21, paradox_frame_event_id((uint8_t)(0x54 | low)));
        /* CategoryId grows by 16 for each step of those two bits. */
        TEST_ASSERT_EQUAL_UINT8(1 + (uint8_t)(low << 4),
                                paradox_frame_category_id((uint8_t)(0x54 | low), 0x10));
    }
}

/* Round-trip: the inverse bit math used by the fixtures must agree with the
 * decoder for every representable (event, category) pair. */
void test_frame_round_trip(void)
{
    for (uint8_t event_id = 1; event_id <= 30; event_id++) {
        if (paradox_event_class(event_id) == PARADOX_CLS_UNKNOWN) {
            continue;
        }
        for (uint8_t category_id = 0; category_id <= 63; category_id++) {
            uint8_t frame[PARADOX_FRAME_LEN];
            make_frame(frame, event_id, category_id);
            TEST_ASSERT_EQUAL_UINT8(event_id, paradox_frame_event_id(frame[0]));
            TEST_ASSERT_EQUAL_UINT8(category_id,
                                    paradox_frame_category_id(frame[0], frame[1]));
        }
    }
}

/* Category names come from the reference table; NULL means "numeric", which is
 * how zones / access codes / remotes are rendered by the caller. */
void test_category_names(void)
{
    TEST_ASSERT_EQUAL_STRING("AC Loss", paradox_category_name(PARADOX_CLS_TROUBLE, 1));
    TEST_ASSERT_EQUAL_STRING("Battery Failure", paradox_category_name(PARADOX_CLS_TROUBLE, 2));
    TEST_ASSERT_EQUAL_STRING("System Ready",
                             paradox_category_name(PARADOX_CLS_PARTITION_STATUS, 1));
    TEST_ASSERT_EQUAL_STRING("Duress Alarm",
                             paradox_category_name(PARADOX_CLS_SPECIAL_ALARM, 5));
    TEST_ASSERT_EQUAL_STRING("Midnight", paradox_category_name(PARADOX_CLS_NON_REPORT, 7));
    TEST_ASSERT_NULL(paradox_category_name(PARADOX_CLS_ZONE, 3));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_all_golden_vectors_decode);
    RUN_TEST(test_never_used_event_ids_are_unknown);
    RUN_TEST(test_zone_and_fire_semantics);
    RUN_TEST(test_bit_math_matches_reference);
    RUN_TEST(test_low_nibble_of_byte1_ignored);
    RUN_TEST(test_frame_round_trip);
    RUN_TEST(test_category_names);
    return UNITY_END();
}
