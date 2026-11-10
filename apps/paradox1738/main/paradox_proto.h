/*
 * Paradox Spectra 1738 serial protocol: pure decode logic.
 *
 * Deliberately free of any ESP-IDF or driver dependency so it can be unit
 * tested on the host (test/host) and reused by the web debug endpoint.
 *
 * Wire format (see https://github.com/LeivoSepp/Paradox-Spectra-1738-SerialOutput):
 * every event is exactly 4 bytes, with no framing header or checksum.
 *   byte 1-2 : one 16-bit field; bits 11-16 = EventId, bits 5-10 = CategoryId
 *   byte 3-4 : panel clock (BCD time), ignored here
 *
 * Because there is no header, a single lost byte permanently misaligns the
 * stream. The decoder therefore validates each candidate frame and slides by
 * one byte until it finds a plausible one again.
 */
#ifndef PARADOX_PROTO_H
#define PARADOX_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PARADOX_FRAME_LEN 4
#define PARADOX_MAX_ZONES 16

/* EventIds that the Spectra 1738 can emit. 0 and 4 are never used, which is
 * part of what makes a misaligned frame detectable. */
typedef enum {
    PARADOX_EVT_NONE = 0,
    PARADOX_EVT_ZONE_OK = 1,
    PARADOX_EVT_ZONE_OPEN = 2,
    PARADOX_EVT_PARTITION_STATUS = 3,
    PARADOX_EVT_NON_REPORT = 5,
    PARADOX_EVT_REMOTE_ARM_DISARM = 6,
    PARADOX_EVT_REMOTE_BTN_B = 7,
    PARADOX_EVT_REMOTE_BTN_C = 8,
    PARADOX_EVT_REMOTE_BTN_D = 9,
    PARADOX_EVT_BYPASS_PROGRAMMING = 10,
    PARADOX_EVT_USER_PGM = 11,
    PARADOX_EVT_ZONE_DELAYED_BREACH = 12,
    PARADOX_EVT_ARM = 13,
    PARADOX_EVT_SPECIAL_ARM = 14,
    PARADOX_EVT_DISARM = 15,
    PARADOX_EVT_DISARM_AFTER_ALARM = 16,
    PARADOX_EVT_CANCEL_ALARM = 17,
    PARADOX_EVT_SPECIAL_DISARM = 18,
    PARADOX_EVT_ZONE_BYPASSED = 19,
    PARADOX_EVT_ZONE_ALARM = 20,
    PARADOX_EVT_FIRE_ALARM = 21,
    PARADOX_EVT_ZONE_ALARM_RESTORE = 22,
    PARADOX_EVT_FIRE_RESTORE = 23,
    PARADOX_EVT_SPECIAL_ALARM = 24,
    PARADOX_EVT_AUTO_ZONE_SHUTDOWN = 25,
    PARADOX_EVT_ZONE_TAMPER = 26,
    PARADOX_EVT_ZONE_TAMPER_RESTORE = 27,
    PARADOX_EVT_SYSTEM_TROUBLE = 28,
    PARADOX_EVT_SYSTEM_TROUBLE_RESTORE = 29,
    PARADOX_EVT_SPECIAL_REPORT = 30,
} paradox_event_id_t;

/* Which family of CategoryId values applies to an event. */
typedef enum {
    PARADOX_CLS_UNKNOWN = 0,
    PARADOX_CLS_ZONE,             /* CategoryId is a zone number 1..16        */
    PARADOX_CLS_PARTITION_STATUS, /* CategoryId is a partition state          */
    PARADOX_CLS_NON_REPORT,
    PARADOX_CLS_REMOTE,           /* CategoryId is a remote control 1..8      */
    PARADOX_CLS_ACCESS_CODE,      /* CategoryId is a user code 1..48          */
    PARADOX_CLS_SPECIAL_ALARM,
    PARADOX_CLS_SPECIAL_ARM,
    PARADOX_CLS_SPECIAL_DISARM,
    PARADOX_CLS_TROUBLE,
    PARADOX_CLS_SPECIAL_REPORT,
} paradox_class_t;

/* Coarse classification used by the HomeKit layer to pick an accessory and a
 * characteristic value. */
typedef enum {
    PARADOX_STATE_IGNORE = 0,
    PARADOX_STATE_OPEN,        /* zone/contact is open (or in alarm)     */
    PARADOX_STATE_CLOSED,      /* zone/contact is closed                 */
    PARADOX_STATE_FIRE,        /* smoke/heat detector in fire alarm      */
    PARADOX_STATE_FIRE_RESTORE,
    PARADOX_STATE_TAMPER,
    PARADOX_STATE_TAMPER_RESTORE,
    PARADOX_STATE_ARMED,
    PARADOX_STATE_DISARMED,
    PARADOX_STATE_TROUBLE,
    PARADOX_STATE_TROUBLE_RESTORE,
} paradox_state_t;

typedef struct {
    uint8_t event_id;      /* 1..30                                  */
    uint8_t category_id;   /* meaning depends on cls                 */
    paradox_class_t cls;
    paradox_state_t state;
    bool zone_open;        /* true for the "open/alarm" side of a zone event */
    uint16_t raw;          /* bytes 1-2 as received, for logging     */
} paradox_event_t;

typedef struct {
    uint32_t frames_ok;        /* frames accepted                         */
    uint32_t framing_errors;   /* misalignments detected (slides + flushes)*/
    uint32_t bytes_discarded;  /* bytes skipped while resyncing            */
    uint32_t overruns;         /* decoded events dropped for lack of room  */
} paradox_stats_t;

/* Stream decoder. Feed it arbitrary byte chunks; it buffers the partial tail
 * and any events the caller could not take yet, so no frame is ever lost. */
#define PARADOX_PENDING_MAX 16

typedef struct {
    uint8_t buf[PARADOX_FRAME_LEN];
    size_t buffered;
    bool aligning;             /* true while hunting for the frame phase */
    paradox_event_t pending[PARADOX_PENDING_MAX];
    size_t pending_head;
    size_t pending_count;
    paradox_stats_t stats;
} paradox_decoder_t;

void paradox_decoder_init(paradox_decoder_t *dec);

/**
 * Push raw bytes and collect complete events.
 *
 * All input bytes are always consumed. Events that do not fit in out[] are
 * kept for paradox_decoder_pop(), so a caller that only wants one event at a
 * time never loses traffic.
 *
 * @return number of events written to out[] (0..max_out)
 */
size_t paradox_decoder_push(paradox_decoder_t *dec, const uint8_t *bytes, size_t len,
                            paradox_event_t *out, size_t max_out);

/**
 * Take one previously decoded event.
 *
 * @return true if an event was available
 */
bool paradox_decoder_pop(paradox_decoder_t *dec, paradox_event_t *out);

/* Number of decoded events waiting to be taken. */
size_t paradox_decoder_pending(const paradox_decoder_t *dec);

/* Pure bit math from the reference implementation, exposed for testing. */
uint8_t paradox_frame_event_id(uint8_t b0);
uint8_t paradox_frame_category_id(uint8_t b0, uint8_t b1);

/* True if a 4-byte window can be a real frame. Used to detect misalignment. */
bool paradox_frame_plausible(const uint8_t *frame);

paradox_class_t paradox_event_class(uint8_t event_id);
paradox_state_t paradox_event_state(uint8_t event_id, bool *zone_open);
const char *paradox_event_name(uint8_t event_id);
const char *paradox_category_name(paradox_class_t cls, uint8_t category_id);

#ifdef __cplusplus
}
#endif

#endif /* PARADOX_PROTO_H */
