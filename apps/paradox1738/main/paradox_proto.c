/*
 * Pure decode logic for the Paradox Spectra 1738 serial protocol.
 * See paradox_proto.h for the wire format. No ESP-IDF dependencies: this file
 * is compiled both into the firmware and into the host unit tests.
 */
#include "paradox_proto.h"

#include <string.h>

/*
 * Bit layout of bytes 1-2 (MSB first), copied from the reference project:
 *
 *   b0: 0b EEEE EECC      bits 1-4  unused
 *                          bits 5-6 low two bits of CategoryId
 *                          bits 7-8 EventId bit 0..1 ... actually:
 *   EventId    = b0 >> 2              (6 bits, values 1..30)
 *   CategoryId = ((b0 & 3) << 4) + (b1 >> 4)   (7 bits, values 0..48)
 *
 * Note the reference demo code sets IsZoneOpen when EventId == 1, which
 * contradicts its own table (1 = "Zone OK", 2 = "Zone Open"). The table is
 * taken as authoritative here: 1 means closed, 2 means open.
 */
uint8_t paradox_frame_event_id(uint8_t b0)
{
    return (uint8_t)(b0 >> 2);
}

uint8_t paradox_frame_category_id(uint8_t b0, uint8_t b1)
{
    return (uint8_t)(((b0 & 0x03u) << 4) | (b1 >> 4));
}

paradox_class_t paradox_event_class(uint8_t event_id)
{
    switch (event_id) {
    case PARADOX_EVT_ZONE_OK:
    case PARADOX_EVT_ZONE_OPEN:
    case PARADOX_EVT_ZONE_DELAYED_BREACH:
    case PARADOX_EVT_ZONE_BYPASSED:
    case PARADOX_EVT_ZONE_ALARM:
    case PARADOX_EVT_FIRE_ALARM:
    case PARADOX_EVT_ZONE_ALARM_RESTORE:
    case PARADOX_EVT_FIRE_RESTORE:
    case PARADOX_EVT_AUTO_ZONE_SHUTDOWN:
    case PARADOX_EVT_ZONE_TAMPER:
    case PARADOX_EVT_ZONE_TAMPER_RESTORE:
        return PARADOX_CLS_ZONE;
    case PARADOX_EVT_PARTITION_STATUS:
        return PARADOX_CLS_PARTITION_STATUS;
    case PARADOX_EVT_NON_REPORT:
        return PARADOX_CLS_NON_REPORT;
    case PARADOX_EVT_REMOTE_ARM_DISARM:
    case PARADOX_EVT_REMOTE_BTN_B:
    case PARADOX_EVT_REMOTE_BTN_C:
    case PARADOX_EVT_REMOTE_BTN_D:
        return PARADOX_CLS_REMOTE;
    case PARADOX_EVT_BYPASS_PROGRAMMING:
    case PARADOX_EVT_USER_PGM:
    case PARADOX_EVT_ARM:
    case PARADOX_EVT_DISARM:
    case PARADOX_EVT_DISARM_AFTER_ALARM:
    case PARADOX_EVT_CANCEL_ALARM:
        return PARADOX_CLS_ACCESS_CODE;
    case PARADOX_EVT_SPECIAL_ALARM:
        return PARADOX_CLS_SPECIAL_ALARM;
    case PARADOX_EVT_SPECIAL_ARM:
        return PARADOX_CLS_SPECIAL_ARM;
    case PARADOX_EVT_SPECIAL_DISARM:
        return PARADOX_CLS_SPECIAL_DISARM;
    case PARADOX_EVT_SYSTEM_TROUBLE:
    case PARADOX_EVT_SYSTEM_TROUBLE_RESTORE:
        return PARADOX_CLS_TROUBLE;
    case PARADOX_EVT_SPECIAL_REPORT:
        return PARADOX_CLS_SPECIAL_REPORT;
    default:
        /* 0 and 4 are never emitted; anything else is unknown. */
        return PARADOX_CLS_UNKNOWN;
    }
}

paradox_state_t paradox_event_state(uint8_t event_id, bool *zone_open)
{
    bool open = false;
    paradox_state_t state = PARADOX_STATE_IGNORE;

    switch (event_id) {
    case PARADOX_EVT_ZONE_OK:
        state = PARADOX_STATE_CLOSED;
        break;
    case PARADOX_EVT_ZONE_OPEN:
    case PARADOX_EVT_ZONE_DELAYED_BREACH:
    case PARADOX_EVT_ZONE_ALARM:
        open = true;
        state = PARADOX_STATE_OPEN;
        break;
    case PARADOX_EVT_ZONE_ALARM_RESTORE:
        state = PARADOX_STATE_CLOSED;
        break;
    case PARADOX_EVT_FIRE_ALARM:
        open = true;
        state = PARADOX_STATE_FIRE;
        break;
    case PARADOX_EVT_FIRE_RESTORE:
        state = PARADOX_STATE_FIRE_RESTORE;
        break;
    case PARADOX_EVT_ZONE_TAMPER:
        open = true;
        state = PARADOX_STATE_TAMPER;
        break;
    case PARADOX_EVT_ZONE_TAMPER_RESTORE:
        state = PARADOX_STATE_TAMPER_RESTORE;
        break;
    case PARADOX_EVT_ARM:
    case PARADOX_EVT_SPECIAL_ARM:
    case PARADOX_EVT_REMOTE_BTN_B: /* remote button B is the arm key */
        state = PARADOX_STATE_ARMED;
        break;
    case PARADOX_EVT_DISARM:
    case PARADOX_EVT_DISARM_AFTER_ALARM:
    case PARADOX_EVT_SPECIAL_DISARM:
    case PARADOX_EVT_REMOTE_BTN_C: /* remote button C is the disarm key */
        state = PARADOX_STATE_DISARMED;
        break;
    case PARADOX_EVT_SYSTEM_TROUBLE:
        state = PARADOX_STATE_TROUBLE;
        break;
    case PARADOX_EVT_SYSTEM_TROUBLE_RESTORE:
        state = PARADOX_STATE_TROUBLE_RESTORE;
        break;
    default:
        break;
    }

    if (zone_open) {
        *zone_open = open;
    }
    return state;
}

const char *paradox_event_name(uint8_t event_id)
{
    switch (event_id) {
    case PARADOX_EVT_ZONE_OK:               return "Zone OK";
    case PARADOX_EVT_ZONE_OPEN:             return "Zone Open";
    case PARADOX_EVT_PARTITION_STATUS:      return "Partition Status";
    case PARADOX_EVT_NON_REPORT:            return "Non-Reportable";
    case PARADOX_EVT_REMOTE_ARM_DISARM:     return "Arm/Disarm Remote";
    case PARADOX_EVT_REMOTE_BTN_B:          return "Remote Button B";
    case PARADOX_EVT_REMOTE_BTN_C:          return "Remote Button C";
    case PARADOX_EVT_REMOTE_BTN_D:          return "Remote Button D";
    case PARADOX_EVT_BYPASS_PROGRAMMING:    return "Bypass Programming";
    case PARADOX_EVT_USER_PGM:              return "User Activated PGM";
    case PARADOX_EVT_ZONE_DELAYED_BREACH:   return "Zone Delayed Breach";
    case PARADOX_EVT_ARM:                   return "Arm";
    case PARADOX_EVT_SPECIAL_ARM:           return "Special Arm";
    case PARADOX_EVT_DISARM:                return "Disarm";
    case PARADOX_EVT_DISARM_AFTER_ALARM:    return "Disarm After Alarm";
    case PARADOX_EVT_CANCEL_ALARM:          return "Cancel Alarm";
    case PARADOX_EVT_SPECIAL_DISARM:        return "Special Disarm";
    case PARADOX_EVT_ZONE_BYPASSED:         return "Zone Bypassed";
    case PARADOX_EVT_ZONE_ALARM:            return "Zone In Alarm";
    case PARADOX_EVT_FIRE_ALARM:            return "Fire Alarm";
    case PARADOX_EVT_ZONE_ALARM_RESTORE:    return "Zone Alarm Restore";
    case PARADOX_EVT_FIRE_RESTORE:          return "Fire Alarm Restore";
    case PARADOX_EVT_SPECIAL_ALARM:         return "Special Alarm";
    case PARADOX_EVT_AUTO_ZONE_SHUTDOWN:    return "Auto Zone Shutdown";
    case PARADOX_EVT_ZONE_TAMPER:           return "Zone Tamper";
    case PARADOX_EVT_ZONE_TAMPER_RESTORE:   return "Zone Tamper Restore";
    case PARADOX_EVT_SYSTEM_TROUBLE:        return "System Trouble";
    case PARADOX_EVT_SYSTEM_TROUBLE_RESTORE:return "System Trouble Restore";
    case PARADOX_EVT_SPECIAL_REPORT:        return "Special Reporting";
    default:                                return "Unknown";
    }
}

const char *paradox_category_name(paradox_class_t cls, uint8_t category_id)
{
    switch (cls) {
    case PARADOX_CLS_PARTITION_STATUS:
        switch (category_id) {
        case 0:  return "System Not Ready";
        case 1:  return "System Ready";
        case 2:  return "Steady Alarm";
        case 3:  return "Pulsed Alarm";
        case 4:  return "Pulsed Or Steady Alarm";
        case 5:  return "Alarm Restored";
        case 6:  return "Bell Squawk Activated";
        case 7:  return "Bell Squawk Deactivated";
        case 8:  return "Ground Start";
        case 9:  return "Disarm Partition";
        case 10: return "Arm Partition";
        case 11: return "Entry Delay Started";
        default: return "Unknown Status";
        }
    case PARADOX_CLS_NON_REPORT:
        switch (category_id) {
        case 0: return "Telephone Line Trouble";
        case 1: return "Reset Smoke Detectors";
        case 2: return "Instant Arming";
        case 3: return "Stay Arming";
        case 4: return "Force Arming";
        case 5: return "Fast Exit";
        case 6: return "PC Fail To Communicate";
        case 7: return "Midnight";
        default: return "Unknown Non-Reportable";
        }
    case PARADOX_CLS_SPECIAL_ARM:
        switch (category_id) {
        case 0: return "Auto Arming";
        case 1: return "Late To Close";
        case 2: return "No Movement Auto-Arming";
        case 3: return "Partial Arming";
        case 4: return "One-Touch Arming";
        case 5: return "Arm With WinLoad";
        case 7: return "Closing Delinquency";
        default: return "Unknown Special Arm";
        }
    case PARADOX_CLS_SPECIAL_DISARM:
        switch (category_id) {
        case 0: return "Cancel Auto Arm";
        case 1: return "Disarm With WinLoad";
        case 2: return "Disarm After Alarm With WinLoad";
        case 3: return "Cancel Alarm With WinLoad";
        default: return "Unknown Special Disarm";
        }
    case PARADOX_CLS_SPECIAL_ALARM:
        switch (category_id) {
        case 0: return "Emergency Alarm";
        case 1: return "Auxiliary Alarm";
        case 2: return "Fire Alarm";
        case 3: return "Recent Closing";
        case 4: return "Auto Zone Shutdown";
        case 5: return "Duress Alarm";
        case 6: return "Keypad Lockout";
        default: return "Unknown Special Alarm";
        }
    case PARADOX_CLS_TROUBLE:
        switch (category_id) {
        case 1:  return "AC Loss";
        case 2:  return "Battery Failure";
        case 3:  return "Auxiliary Overload";
        case 4:  return "Bell Overload";
        case 5:  return "Bell Disconnected";
        case 6:  return "Timer Loss";
        case 7:  return "Fire Loop Trouble";
        case 8:  return "Future Use";
        case 9:  return "Module Fault";
        case 10: return "Printer Fault";
        case 11: return "Fail To Communicate";
        default: return "Unknown Trouble";
        }
    case PARADOX_CLS_SPECIAL_REPORT:
        switch (category_id) {
        case 0: return "System Power Up";
        case 1: return "Test Report";
        case 2: return "WinLoad Access";
        case 3: return "WinLoad Access Granted";
        default: return "Unknown Reporting";
        }
    case PARADOX_CLS_ACCESS_CODE:
    case PARADOX_CLS_REMOTE:
    case PARADOX_CLS_ZONE:
    case PARADOX_CLS_UNKNOWN:
    default:
        /* Numeric categories: the caller formats them as Zone N / Code NNN. */
        return NULL;
    }
}

/*
 * Without a header or checksum, the only way to spot misalignment is to check
 * that the decoded fields are ones the panel could actually send:
 *   - EventId must be a known event (never 0 or 4)
 *   - CategoryId must fit the range used by that event
 * A wrong phase almost always fails this, and sliding one byte at a time
 * recovers alignment within a frame or two.
 */
static bool category_in_range(paradox_class_t cls, uint8_t category_id)
{
    switch (cls) {
    case PARADOX_CLS_ZONE:
        return category_id >= 1 && category_id <= PARADOX_MAX_ZONES;
    case PARADOX_CLS_PARTITION_STATUS:
        return category_id <= 11;
    case PARADOX_CLS_NON_REPORT:
        return category_id <= 7;
    case PARADOX_CLS_REMOTE:
        return category_id >= 1 && category_id <= 8;
    case PARADOX_CLS_ACCESS_CODE:
        return category_id >= 1 && category_id <= 48;
    case PARADOX_CLS_SPECIAL_ALARM:
        return category_id <= 6;
    case PARADOX_CLS_SPECIAL_ARM:
        return category_id <= 7;
    case PARADOX_CLS_SPECIAL_DISARM:
        return category_id <= 3;
    case PARADOX_CLS_TROUBLE:
        return category_id >= 1 && category_id <= 11;
    case PARADOX_CLS_SPECIAL_REPORT:
        return category_id <= 5;
    case PARADOX_CLS_UNKNOWN:
    default:
        return false;
    }
}

/*
 * Bytes 3-4 carry the panel clock, which gives us a second independent check on
 * alignment. Reference layout (verified against its worked examples: 23:59 is
 * 0xBB 0xB0 and 20:00 is 0xA0 0x00):
 *   b2 = HHHHH xHmm   (bit 2 is unused by the reference formula)
 *   b3 = mmmm 0000   (low nibble always zero)
 *   hour   = b2 >> 3              must be 0..23
 *   minute = ((b2 & 3) << 4) + (b3 >> 4)  must be 0..59
 */
static bool clock_plausible(uint8_t b2, uint8_t b3)
{
    if (b3 & 0x0Fu) {
        return false;
    }
    uint8_t hour = (uint8_t)(b2 >> 3);
    uint8_t minute = (uint8_t)(((b2 & 0x03u) << 4) | (b3 >> 4));
    return hour <= 23 && minute <= 59;
}

bool paradox_frame_plausible(const uint8_t *frame)
{
    uint8_t event_id = paradox_frame_event_id(frame[0]);
    paradox_class_t cls = paradox_event_class(event_id);

    if (cls == PARADOX_CLS_UNKNOWN) {
        return false;
    }
    if (!category_in_range(cls, paradox_frame_category_id(frame[0], frame[1]))) {
        return false;
    }
    return clock_plausible(frame[2], frame[3]);
}

void paradox_decoder_init(paradox_decoder_t *dec)
{
    memset(dec, 0, sizeof(*dec));
}

static void decode_into(const uint8_t *frame, paradox_event_t *evt)
{
    uint8_t event_id = paradox_frame_event_id(frame[0]);

    evt->event_id = event_id;
    evt->category_id = paradox_frame_category_id(frame[0], frame[1]);
    evt->cls = paradox_event_class(event_id);
    evt->state = paradox_event_state(event_id, &evt->zone_open);
    evt->raw = (uint16_t)((frame[0] << 8) | frame[1]);
}

size_t paradox_decoder_pending(const paradox_decoder_t *dec)
{
    return dec->pending_count;
}

bool paradox_decoder_pop(paradox_decoder_t *dec, paradox_event_t *out)
{
    if (dec->pending_count == 0) {
        return false;
    }
    *out = dec->pending[dec->pending_head];
    dec->pending_head = (dec->pending_head + 1) % PARADOX_PENDING_MAX;
    dec->pending_count--;
    return true;
}

/* Hand one decoded frame to the caller's array, or queue it if there is no room
 * left in this call. */
static void deliver(paradox_decoder_t *dec, const uint8_t *frame,
                    paradox_event_t *out, size_t max_out, size_t *produced)
{
    paradox_event_t evt;

    decode_into(frame, &evt);
    dec->stats.frames_ok++;

    if (*produced < max_out) {
        out[(*produced)++] = evt;
        return;
    }
    if (dec->pending_count == PARADOX_PENDING_MAX) {
        /* Caller is not draining fast enough: drop the newest event rather than
         * disturb the order of the ones already queued. */
        dec->stats.overruns++;
        return;
    }
    size_t tail = (dec->pending_head + dec->pending_count) % PARADOX_PENDING_MAX;
    dec->pending[tail] = evt;
    dec->pending_count++;
}

size_t paradox_decoder_push(paradox_decoder_t *dec, const uint8_t *bytes, size_t len,
                            paradox_event_t *out, size_t max_out)
{
    size_t produced = 0;

    /* At 9600 baud the panel emits at most ~2.5 frames per second, so stepping
     * one byte at a time is more than fast enough and keeps the resync logic in
     * a single place. */
    for (size_t i = 0; i < len; i++) {
        dec->buf[dec->buffered++] = bytes[i];
        if (dec->buffered < PARADOX_FRAME_LEN) {
            continue;
        }

        if (!paradox_frame_plausible(dec->buf)) {
            /* Wrong phase or a corrupted byte: drop the leading byte and try the
             * next window. Always terminates, because each slide consumes one
             * byte of input. */
            memmove(dec->buf, dec->buf + 1, PARADOX_FRAME_LEN - 1);
            dec->buffered = PARADOX_FRAME_LEN - 1;
            if (!dec->aligning) {
                /* Count one error per corruption episode so the debug endpoint
                 * reports how often the link slipped, not how many bytes were
                 * thrown away hunting for the phase. */
                dec->stats.framing_errors++;
                dec->aligning = true;
            }
            dec->stats.bytes_discarded++;
            continue;
        }

        deliver(dec, dec->buf, out, max_out, &produced);
        dec->aligning = false;
        dec->buffered = 0;
    }

    return produced;
}
