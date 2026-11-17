/*
 * Recorded Paradox panel traffic replayed instead of reading UART2, so the
 * firmware can be exercised with no alarm panel attached (T0.4).
 *
 * Enabled with CONFIG_PARADOX_SIMULATOR. Compiles to nothing otherwise.
 */
#include <stdint.h>
#include <stddef.h>
#ifdef ESP_PLATFORM
#include "sdkconfig.h"
#endif
#include "paradox1738.h"

/* Raw 4-byte frames exactly as captured on the panel TX line, in order.
 * Decoding is left to the real decoder so the simulator exercises it too. */
static const uint8_t recorded_frames[][4] = {
    {0x00, 0x10, 0xA0, 0x00}, /* zone 1 closed   (evt 0,  cat 1)  */
    {0x04, 0x10, 0xA0, 0x00}, /* zone 1 open     (evt 1,  cat 1)  */
    {0x54, 0x10, 0xA0, 0x00}, /* zone 1 fire alarm   (evt 21, cat 1) */
    {0x5C, 0x10, 0xA0, 0x00}, /* zone 1 fire restore (evt 23, cat 1) */
    {0x4C, 0x50, 0xA0, 0x00}, /* zone 5 bypassed (evt 19, cat 5) */
    {0x50, 0x50, 0xA0, 0x00}, /* zone 5 alarm (evt 20, cat 5) */
    {0x58, 0x50, 0xA0, 0x00}, /* zone 5 alarm restore (evt 22, cat 5) */
    {0x68, 0x50, 0xA0, 0x00}, /* zone 5 tamper (evt 26, cat 5) */
    {0x6C, 0x50, 0xA0, 0x00}, /* zone 5 tamper restore (evt 27, cat 5) */
    {0x70, 0x70, 0xA0, 0x00}, /* fire loop trouble (evt 28, cat 7) */
    {0x74, 0x70, 0xA0, 0x00}, /* fire loop trouble restore (evt 29, cat 7) */
    {0x00, 0x20, 0xA0, 0x00}, /* zone 2 closed   (evt 0,  cat 2)  */
    {0x04, 0x20, 0xA0, 0x00}, /* zone 2 open     (evt 1,  cat 2)  */
    {0x00, 0x30, 0xA0, 0x00}, /* zone 3 closed   (evt 0,  cat 3)  */
    {0x04, 0x30, 0xA0, 0x00}, /* zone 3 open     (evt 1,  cat 3)  */
    {0x00, 0x40, 0xA0, 0x00}, /* zone 4 closed   (evt 0,  cat 4)  */
    {0x04, 0x40, 0xA0, 0x00}, /* zone 4 open     (evt 1,  cat 4)  */
    {0x00, 0x50, 0xA0, 0x00}, /* zone 5 closed   (evt 0,  cat 5)  */
    {0x04, 0x50, 0xA0, 0x00}, /* zone 5 open     (evt 1,  cat 5)  */
    {0x00, 0x60, 0xA0, 0x00}, /* zone 6 closed   (evt 0,  cat 6)  */
    {0x04, 0x60, 0xA0, 0x00}, /* zone 6 open     (evt 1,  cat 6)  */
    {0x00, 0x70, 0xA0, 0x00}, /* zone 7 closed   (evt 0,  cat 7)  */
    {0x04, 0x70, 0xA0, 0x00}, /* zone 7 open     (evt 1,  cat 7)  */
    {0x00, 0x80, 0xA0, 0x00}, /* zone 8 closed   (evt 0,  cat 8)  */
    {0x04, 0x80, 0xA0, 0x00}, /* zone 8 open     (evt 1,  cat 8)  */
    {0x00, 0x90, 0xA0, 0x00}, /* zone 9 closed   (evt 0,  cat 9)  */
    {0x04, 0x90, 0xA0, 0x00}, /* zone 9 open     (evt 1,  cat 9)  */
    {0x00, 0xA0, 0xA0, 0x00}, /* zone 10 closed  (evt 0,  cat 10) */
    {0x04, 0xA0, 0xA0, 0x00}, /* zone 10 open    (evt 1,  cat 10) */
    {0x00, 0xB0, 0xA0, 0x00}, /* zone 11 closed  (evt 0,  cat 11) */
    {0x04, 0xB0, 0xA0, 0x00}, /* zone 11 open    (evt 1,  cat 11) */
    {0x00, 0xC0, 0xA0, 0x00}, /* zone 12 closed  (evt 0,  cat 12) */
    {0x04, 0xC0, 0xA0, 0x00}, /* zone 12 open    (evt 1,  cat 12) */
    {0x00, 0xD0, 0xA0, 0x00}, /* zone 13 closed  (evt 0,  cat 13) */
    {0x04, 0xD0, 0xA0, 0x00}, /* zone 13 open    (evt 1,  cat 13) */
    {0x00, 0xE0, 0xA0, 0x00}, /* zone 14 closed  (evt 0,  cat 14) */
    {0x04, 0xE0, 0xA0, 0x00}, /* zone 14 open    (evt 1,  cat 14) */
    {0x00, 0xF0, 0xA0, 0x00}, /* zone 15 closed  (evt 0,  cat 15) */
    {0x04, 0xF0, 0xA0, 0x00}, /* zone 15 open    (evt 1,  cat 15) */
    /* Category 16 needs the carry bit: it lives in bits 5-6 of byte 0. */
    {0x01, 0x00, 0xA0, 0x00}, /* zone 16 closed  (evt 0,  cat 16) */
    {0x05, 0x00, 0xA0, 0x00}, /* zone 16 open    (evt 1,  cat 16) */
    {0x54, 0x20, 0xA0, 0x00}, /* fire alarm      (evt 21, cat 2)  */
    {0x5C, 0x20, 0xA0, 0x00}, /* fire restore    (evt 23, cat 2)  */
    {0x60, 0x20, 0xA0, 0x00}, /* special alarm   (evt 24, cat 2: fire via keys) */
};

#define REPLAY_INTERVAL_MS 2000

const uint8_t (*paradox_sim_recorded_frames(void))[4]
{
    return recorded_frames;
}

size_t paradox_sim_recorded_frame_count(void)
{
    return sizeof(recorded_frames) / sizeof(recorded_frames[0]);
}

size_t paradox_sim_replay_interval_ms(void)
{
    return REPLAY_INTERVAL_MS;
}

size_t paradox_sim_test_interval_ms(void)
{
    return 150;
}
