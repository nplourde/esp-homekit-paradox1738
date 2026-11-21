/* paradox_audit - a fixed-size record of the operations the bridge performed.
 *
 * Pure logic on purpose: no ESP-IDF headers, no allocation, no clock calls. The
 * caller supplies the timestamp, which keeps the whole thing testable on a PC and
 * keeps this module free of the question of whether the device even has a wall
 * clock (it does not: there is no SNTP client here, so the honest time base is
 * uptime).
 *
 * Why this exists before there is anything to control: M9 wants a bridge-side
 * record of every command so that a bridge-initiated arm can be reconciled with
 * what the panel itself logged, including which user code the panel thought it
 * saw. A record written at the moment a command is *accepted* is worth little;
 * the value is in also holding the refusals, because "someone tried to disarm the
 * house and was refused" is the entry that matters. The web layer therefore funnels
 * every refusal it makes through here, which means a refusal cannot be recorded
 * incorrectly by being recorded in the wrong place - the place is the single
 * function that sends an error to a client.
 *
 * Two rules that are load-bearing and tested:
 *  - the record is a fixed static ring, so a hostile client cannot make the trail
 *    grow, and running out of memory cannot make the recording of a security
 *    event fail;
 *  - every field is sanitized to printable ASCII on the way in, and the JSON
 *    writer escapes, so a client-supplied path or User-Agent cannot forge a
 *    record boundary or produce a body that does not parse. That is the same
 *    class of defect that send_error() once had.
 */
#ifndef PARADOX_AUDIT_H
#define PARADOX_AUDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* How far back the trail reaches. Deep enough that a full robustness battery
 * leaves a readable footprint, shallow enough to be static memory that can never
 * fail to allocate: 16 entries is about 2.6 KiB of .bss. */
#define PARADOX_AUDIT_ENTRIES 16u

#define PARADOX_AUDIT_PRINCIPAL_MAX 16
#define PARADOX_AUDIT_METHOD_MAX 8
#define PARADOX_AUDIT_PATH_MAX 64
#define PARADOX_AUDIT_STATUS_MAX 8
#define PARADOX_AUDIT_NOTE_MAX 48

typedef struct {
    /* Monotonic across the boot. Never reused, so a reader that saw sequence N
     * and now sees the oldest entry as N+9 knows that entries went away, rather
     * than assuming the trail is complete. */
    uint32_t sequence;
    /* Milliseconds of uptime, because the device has no wall clock. The panel's
     * own audit trail is on the panel and is keyed by user code, not by our
     * clock, so uptime is the honest correlation base on this side. */
    int64_t uptime_ms;
    /* Who the bridge believed was asking: "admin", "operator" or "anonymous".
     * Today there is only one credential, so the honest answer is admin or
     * anonymous; the field exists now so the split does not have to retrofit a
     * trail that recorded nobody. */
    char principal[PARADOX_AUDIT_PRINCIPAL_MAX];
    char method[PARADOX_AUDIT_METHOD_MAX];
    char path[PARADOX_AUDIT_PATH_MAX];
    char status[PARADOX_AUDIT_STATUS_MAX];
    /* Extra context the request line cannot carry: which zone, which code the
     * panel reported, why a confirm was missing. Never a request body. */
    char note[PARADOX_AUDIT_NOTE_MAX];
} paradox_audit_entry_t;

/* Clears the trail and restarts the sequence counter. Call at boot. */
void paradox_audit_reset(void);

/* Records one operation. Timestamps and outcome come from the caller; the only
 * thing this module decides is how the text is contained. A field that is too
 * long is truncated and marked with a trailing '~' so a reader cannot mistake a
 * cut string for a complete one. NULL fields are stored as empty strings rather
 * than dropping the record. */
void paradox_audit_record(int64_t uptime_ms, const char *principal,
                          const char *method, const char *path,
                          const char *status, const char *note);

/* Number of records currently held, oldest to newest is implied by sequence. */
size_t paradox_audit_count(void);

/* The next sequence number that paradox_audit_record() will hand out. A client
 * polling the trail can compare this with the sequences it saw to notice that
 * records aged out between polls. */
uint32_t paradox_audit_next_sequence(void);

/* Copies up to max records, newest first, and returns how many were copied. */
size_t paradox_audit_snapshot(paradox_audit_entry_t *out, size_t max);

/* Renders the whole trail, newest first, as a JSON document into buffer and
 * returns its length; the buffer needs one byte more than that length for the
 * terminator. A smaller buffer is refused: the call returns 0 and leaves buffer
 * as an empty string, so a caller can report a clean failure rather than send
 * JSON that stops in the middle of a field. */
size_t paradox_audit_json(char *buffer, size_t buffer_size);

#endif /* PARADOX_AUDIT_H */
