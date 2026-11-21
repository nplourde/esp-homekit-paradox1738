/* paradox_audit - see paradox_audit.h for the reasoning. */

#include "paradox_audit.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static paradox_audit_entry_t s_entries[PARADOX_AUDIT_ENTRIES];
static size_t s_count;
static size_t s_next_slot;
static uint32_t s_next_sequence = 1u;

/* The web server runs handlers on one task, but the panel task is going to
 * record the commands it sends and the events it reads back, so two writers do
 * exist in the plan even though only one exists today. A torn record is not just
 * cosmetic: it could render a string with no terminator. A spin lock is the right
 * size here because the critical section is a memory copy of a few dozen bytes
 * and is never held across I/O, so it cannot be held long enough to matter. */
static atomic_flag s_lock = ATOMIC_FLAG_INIT;

static void audit_lock(void)
{
    while (atomic_flag_test_and_set_explicit(&s_lock, memory_order_acquire)) {
        /* Spin. Nothing else runs at this priority inside this section. */
    }
}

static void audit_unlock(void)
{
    atomic_flag_clear_explicit(&s_lock, memory_order_release);
}

/* Copies text into a fixed field, keeping only printable ASCII and replacing
 * anything else with '.' so that an attempt to smuggle a control byte or a
 * non-ASCII byte leaves a visible mark instead of disappearing. A value that did
 * not fit ends in '~' so a reader is never shown a truncated string that looks
 * complete. */
static void store_field(char *field, size_t field_size, const char *value)
{
    size_t out = 0;
    bool truncated = false;

    field[0] = '\0';
    if (value == NULL) {
        return;
    }
    for (size_t in = 0; value[in] != '\0'; ++in) {
        const char printable = (value[in] >= ' ' && value[in] <= '~') ? value[in] : '.';
        /* Stop two bytes early: a truncated value spends one byte on the mark,
         * and the field still needs its terminator inside its own bounds. */
        if (out + 2u >= field_size) {
            truncated = true;
            break;
        }
        field[out++] = printable;
    }
    if (truncated) {
        field[out++] = '~';
    }
    field[out] = '\0';
}

void paradox_audit_reset(void)
{
    audit_lock();
    memset(s_entries, 0, sizeof(s_entries));
    s_count = 0;
    s_next_slot = 0;
    s_next_sequence = 1u;
    audit_unlock();
}

void paradox_audit_record(int64_t uptime_ms, const char *principal,
                          const char *method, const char *path,
                          const char *status, const char *note)
{
    audit_lock();
    paradox_audit_entry_t *entry = &s_entries[s_next_slot];
    memset(entry, 0, sizeof(*entry));
    entry->sequence = s_next_sequence++;
    entry->uptime_ms = uptime_ms;
    store_field(entry->principal, sizeof(entry->principal), principal);
    store_field(entry->method, sizeof(entry->method), method);
    store_field(entry->path, sizeof(entry->path), path);
    store_field(entry->status, sizeof(entry->status), status);
    store_field(entry->note, sizeof(entry->note), note);

    s_next_slot = (s_next_slot + 1u) % PARADOX_AUDIT_ENTRIES;
    if (s_count < PARADOX_AUDIT_ENTRIES) {
        ++s_count;
    }
    audit_unlock();
}

size_t paradox_audit_count(void)
{
    return s_count;
}

uint32_t paradox_audit_next_sequence(void)
{
    return s_next_sequence;
}

size_t paradox_audit_snapshot(paradox_audit_entry_t *out, size_t max)
{
    if (out == NULL || max == 0u) {
        return 0u;
    }
    audit_lock();
    /* s_next_slot points at the oldest record once the ring is full, and one
     * past the newest record while it is still filling. Either way the newest
     * record is one before s_next_slot, walking backwards. */
    size_t copied = s_count < max ? s_count : max;
    for (size_t i = 0; i < copied; ++i) {
        size_t back = (s_next_slot + PARADOX_AUDIT_ENTRIES - 1u - i) %
                      PARADOX_AUDIT_ENTRIES;
        out[i] = s_entries[back];
    }
    audit_unlock();
    return copied;
}

/* Appends to the document, reporting failure rather than writing a partial
 * record. The caller turns a failure into a clear HTTP error instead of shipping
 * JSON that stops in the middle of a field. */
typedef struct {
    char *buffer;
    size_t capacity;
    size_t used;
    bool failed;
} json_writer_t;

static void write_text(json_writer_t *writer, const char *text)
{
    const size_t length = strlen(text);
    if (writer->failed || writer->used + length >= writer->capacity) {
        writer->failed = true;
        return;
    }
    memcpy(writer->buffer + writer->used, text, length);
    writer->used += length;
    writer->buffer[writer->used] = '\0';
}

/* The fields are already restricted to printable ASCII, so the only characters
 * that can break a JSON string are the quote and the backslash. Both are legal in
 * a URL path, so they are escaped rather than dropped. */
static void write_field(json_writer_t *writer, const char *value)
{
    for (const char *p = value; *p != '\0'; ++p) {
        char pair[3] = {'\\', 0, 0};
        if (*p == '"' || *p == '\\') {
            pair[1] = *p;
            write_text(writer, pair);
        } else {
            pair[0] = *p;
            write_text(writer, pair);
        }
    }
}

static void write_quoted(json_writer_t *writer, const char *value)
{
    write_text(writer, "\"");
    write_field(writer, value);
    write_text(writer, "\"");
}

size_t paradox_audit_json(char *buffer, size_t buffer_size)
{
    if (buffer == NULL || buffer_size < 32u) {
        return 0u;
    }
    buffer[0] = '\0';
    audit_lock();

    json_writer_t writer = {buffer, buffer_size, 0, false};
    char number[24];

    write_text(&writer, "{\"entries\":[");
    for (size_t i = 0; i < s_count && !writer.failed; ++i) {
        size_t back =
            (s_next_slot + PARADOX_AUDIT_ENTRIES - 1u - i) % PARADOX_AUDIT_ENTRIES;
        const paradox_audit_entry_t *entry = &s_entries[back];

        if (i != 0u) {
            write_text(&writer, ",");
        }
        write_text(&writer, "{\"sequence\":");
        snprintf(number, sizeof(number), "%u", (unsigned)entry->sequence);
        write_text(&writer, number);
        write_text(&writer, ",\"uptime_ms\":");
        snprintf(number, sizeof(number), "%lld", (long long)entry->uptime_ms);
        write_text(&writer, number);
        write_text(&writer, ",\"principal\":");
        write_quoted(&writer, entry->principal);
        write_text(&writer, ",\"method\":");
        write_quoted(&writer, entry->method);
        write_text(&writer, ",\"path\":");
        write_quoted(&writer, entry->path);
        write_text(&writer, ",\"status\":");
        write_quoted(&writer, entry->status);
        write_text(&writer, ",\"note\":");
        write_quoted(&writer, entry->note);
        write_text(&writer, "}");
    }
    write_text(&writer, "],\"next_sequence\":");
    snprintf(number, sizeof(number), "%u", (unsigned)s_next_sequence);
    write_text(&writer, number);
    write_text(&writer, ",\"capacity\":");
    snprintf(number, sizeof(number), "%u", (unsigned)PARADOX_AUDIT_ENTRIES);
    write_text(&writer, number);
    write_text(&writer, "}");

    const size_t length = writer.used;
    const bool failed = writer.failed;
    audit_unlock();

    if (failed) {
        buffer[0] = '\0';
        return 0u;
    }
    return length;
}
