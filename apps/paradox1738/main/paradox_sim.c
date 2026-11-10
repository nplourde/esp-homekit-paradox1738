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

#ifdef CONFIG_PARADOX_SIMULATOR

/* Raw 4-byte frames exactly as captured on the panel TX line, in order.
 * Decoding is left to the real decoder so the simulator exercises it too. */
static const uint8_t recorded_frames[][4] = {
    {0x04, 0x10, 0xA0, 0x00}, /* zone 1 closed   (evt 1,  cat 1)  */
    {0x08, 0x10, 0xA0, 0x00}, /* zone 1 open     (evt 2,  cat 1)  */
    {0x54, 0x10, 0xA0, 0x00}, /* zone 1 fire alarm   (evt 21, cat 1) */
    {0x5C, 0x10, 0xA0, 0x00}, /* zone 1 fire restore (evt 23, cat 1) */
    {0x04, 0x20, 0xA0, 0x00}, /* zone 2 closed   (evt 1,  cat 2)  */
    {0x08, 0x20, 0xA0, 0x00}, /* zone 2 open     (evt 2,  cat 2)  */
    {0x04, 0x30, 0xA0, 0x00}, /* zone 3 closed   (evt 1,  cat 3)  */
    {0x08, 0x30, 0xA0, 0x00}, /* zone 3 open     (evt 2,  cat 3)  */
    {0x04, 0x40, 0xA0, 0x00}, /* zone 4 closed   (evt 1,  cat 4)  */
    {0x08, 0x40, 0xA0, 0x00}, /* zone 4 open     (evt 2,  cat 4)  */
    {0x04, 0x50, 0xA0, 0x00}, /* zone 5 closed   (evt 1,  cat 5)  */
    {0x08, 0x50, 0xA0, 0x00}, /* zone 5 open     (evt 2,  cat 5)  */
    {0x04, 0x60, 0xA0, 0x00}, /* zone 6 closed   (evt 1,  cat 6)  */
    {0x08, 0x60, 0xA0, 0x00}, /* zone 6 open     (evt 2,  cat 6)  */
    {0x04, 0x70, 0xA0, 0x00}, /* zone 7 closed   (evt 1,  cat 7)  */
    {0x08, 0x70, 0xA0, 0x00}, /* zone 7 open     (evt 2,  cat 7)  */
    {0x04, 0x80, 0xA0, 0x00}, /* zone 8 closed   (evt 1,  cat 8)  */
    {0x08, 0x80, 0xA0, 0x00}, /* zone 8 open     (evt 2,  cat 8)  */
    {0x04, 0x90, 0xA0, 0x00}, /* zone 9 closed   (evt 1,  cat 9)  */
    {0x08, 0x90, 0xA0, 0x00}, /* zone 9 open     (evt 2,  cat 9)  */
    {0x04, 0xA0, 0xA0, 0x00}, /* zone 10 closed  (evt 1,  cat 10) */
    {0x08, 0xA0, 0xA0, 0x00}, /* zone 10 open    (evt 2,  cat 10) */
    {0x04, 0xB0, 0xA0, 0x00}, /* zone 11 closed  (evt 1,  cat 11) */
    {0x08, 0xB0, 0xA0, 0x00}, /* zone 11 open    (evt 2,  cat 11) */
    {0x04, 0xC0, 0xA0, 0x00}, /* zone 12 closed  (evt 1,  cat 12) */
    {0x08, 0xC0, 0xA0, 0x00}, /* zone 12 open    (evt 2,  cat 12) */
    {0x04, 0xD0, 0xA0, 0x00}, /* zone 13 closed  (evt 1,  cat 13) */
    {0x08, 0xD0, 0xA0, 0x00}, /* zone 13 open    (evt 2,  cat 13) */
    {0x04, 0xE0, 0xA0, 0x00}, /* zone 14 closed  (evt 1,  cat 14) */
    {0x08, 0xE0, 0xA0, 0x00}, /* zone 14 open    (evt 2,  cat 14) */
    {0x04, 0xF0, 0xA0, 0x00}, /* zone 15 closed  (evt 1,  cat 15) */
    {0x08, 0xF0, 0xA0, 0x00}, /* zone 15 open    (evt 2,  cat 15) */
    /* Category 16 needs the carry bit: it lives in bits 5-6 of byte 0. */
    {0x05, 0x00, 0xA0, 0x00}, /* zone 16 closed  (evt 1,  cat 16) */
    {0x09, 0x00, 0xA0, 0x00}, /* zone 16 open    (evt 2,  cat 16) */
    {0x54, 0x20, 0xA0, 0x00}, /* fire alarm      (evt 21, cat 2)  */
    {0x5C, 0x20, 0xA0, 0x00}, /* fire restore    (evt 23, cat 2)  */
    {0x60, 0x62, 0xA0, 0x00}, /* special alarm   (evt 24, cat 6: fire via keys 7/9) */
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

#endif /* CONFIG_PARADOX_SIMULATOR */
