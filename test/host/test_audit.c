/*
 * Host-side tests for the bridge's record of what it was asked to do.
 *
 * The trail is the piece of the control-plane work that pays for itself before
 * there is anything to control: it already holds the refusals the web layer
 * makes today, and when M9 puts commands on the panel line the same record has
 * to hold the attempts that were refused. The properties worth testing are the
 * unglamorous ones - that a hostile input cannot forge a record or break the
 * JSON, that the ring cannot be made to grow, and that a reader can tell when
 * records aged out instead of assuming the trail is complete.
 */

#include "paradox_audit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "unity_compat.h"

void setUp(void)
{
    paradox_audit_reset();
}

void tearDown(void) {}

static const paradox_audit_entry_t *newest(void)
{
    static paradox_audit_entry_t one;
    paradox_audit_snapshot(&one, 1);
    return &one;
}

/* An empty trail is distinguishable from a full one, and sequence numbers start
 * at 1 so that 0 can stay reserved for "no record seen yet" in a client. */
static void test_reset_leaves_an_empty_trail(void)
{
    TEST_ASSERT_EQUAL_SIZE(0u, paradox_audit_count());
    TEST_ASSERT_EQUAL_UINT32(1u, paradox_audit_next_sequence());
    TEST_ASSERT_EQUAL_SIZE(0u, paradox_audit_snapshot(NULL, 4));

    char json[256];
    size_t length = paradox_audit_json(json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_SIZE(0u, length);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"entries\":[]"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"next_sequence\":1"));
}

static void test_record_keeps_the_fields_apart(void)
{
    paradox_audit_record(1500, "admin", "PUT", "/api/zones/3", "200", "renamed");
    const paradox_audit_entry_t *entry = newest();

    TEST_ASSERT_EQUAL_UINT32(1u, entry->sequence);
    TEST_ASSERT_EQUAL_INT64(1500, entry->uptime_ms);
    TEST_ASSERT_EQUAL_STRING("admin", entry->principal);
    TEST_ASSERT_EQUAL_STRING("PUT", entry->method);
    TEST_ASSERT_EQUAL_STRING("/api/zones/3", entry->path);
    TEST_ASSERT_EQUAL_STRING("200", entry->status);
    TEST_ASSERT_EQUAL_STRING("renamed", entry->note);
    /* One field must not bleed into the next: this is the whole reason the
     * fields are fixed arrays with their own terminators. */
    TEST_ASSERT_EQUAL_SIZE(PARADOX_AUDIT_PATH_MAX, sizeof(entry->path));
}

/* A refusal is as much a record as an acceptance; the request line is the point. */
static void test_refusals_are_records_too(void)
{
    paradox_audit_record(10, "anonymous", "POST", "/api/auth/login", "401", "");
    paradox_audit_record(12, "anonymous", "POST", "/api/homekit/restart", "403", "");
    TEST_ASSERT_EQUAL_SIZE(2u, paradox_audit_count());
    TEST_ASSERT_EQUAL_STRING("403", newest()->status);
    TEST_ASSERT_EQUAL_STRING("anonymous", newest()->principal);
}

/* NULL must not drop the record and must not crash: a handler that has nothing
 * extra to say should still leave the trail entry. */
static void test_null_fields_still_record(void)
{
    paradox_audit_record(7, NULL, NULL, NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_SIZE(1u, paradox_audit_count());
    const paradox_audit_entry_t *entry = newest();
    TEST_ASSERT_EQUAL_STRING("", entry->principal);
    TEST_ASSERT_EQUAL_STRING("", entry->path);
    TEST_ASSERT_EQUAL_STRING("", entry->note);
}

/* The trail is bounded by design: a client spraying requests cannot make it
 * longer, and must not be able to make it bigger either. */
static void test_ring_is_bounded_and_overwrites_oldest(void)
{
    for (unsigned i = 0; i < 40u; ++i) {
        char note[16];
        snprintf(note, sizeof(note), "n%02u", i);
        paradox_audit_record((int64_t)i, "admin", "GET", "/api/audit", "200", note);
    }
    TEST_ASSERT_EQUAL_SIZE(PARADOX_AUDIT_ENTRIES, paradox_audit_count());

    paradox_audit_entry_t all[PARADOX_AUDIT_ENTRIES + 4];
    size_t copied = paradox_audit_snapshot(all, sizeof(all) / sizeof(all[0]));
    TEST_ASSERT_EQUAL_SIZE(PARADOX_AUDIT_ENTRIES, copied);
    /* Newest first: sequences 40 down to 25, and 41 is the next one to come. */
    TEST_ASSERT_EQUAL_UINT32(40u, all[0].sequence);
    TEST_ASSERT_EQUAL_UINT32(25u, all[PARADOX_AUDIT_ENTRIES - 1u].sequence);
    TEST_ASSERT_EQUAL_UINT32(41u, paradox_audit_next_sequence());
    for (size_t i = 1; i < copied; ++i) {
        TEST_ASSERT_EQUAL_UINT32(all[i - 1].sequence - 1u, all[i].sequence);
    }
}

/* A poller that saw sequence N and now finds the oldest at N+9 must know that
 * records went away rather than concluding nothing happened. */
static void test_loss_is_visible_to_a_reader(void)
{
    for (unsigned i = 0; i < 30u; ++i) {
        paradox_audit_record((int64_t)i, "admin", "GET", "/x", "200", "");
    }
    paradox_audit_entry_t all[PARADOX_AUDIT_ENTRIES];
    size_t copied = paradox_audit_snapshot(all, PARADOX_AUDIT_ENTRIES);
    TEST_ASSERT_EQUAL_SIZE(PARADOX_AUDIT_ENTRIES, copied);
    /* The reader of sequences 15..30 was shown 15 records; 1..14 are gone and
     * the only way to know is that next_sequence outruns the oldest present. */
    TEST_ASSERT_EQUAL_UINT32(31u, paradox_audit_next_sequence());
    TEST_ASSERT_TRUE(paradox_audit_next_sequence() - all[copied - 1].sequence > 1u);
}

/* Too-long input is truncated, and marked, so a truncated string can never be
 * mistaken for the whole thing. */
static void test_long_fields_are_marked_as_truncated(void)
{
    char long_path[200];
    memset(long_path, 'a', sizeof(long_path) - 1);
    long_path[sizeof(long_path) - 1] = '\0';
    paradox_audit_record(1, "admin", "PUT", long_path, "200", NULL);

    const paradox_audit_entry_t *entry = newest();
    TEST_ASSERT_EQUAL_SIZE(PARADOX_AUDIT_PATH_MAX - 1u, strlen(entry->path));
    TEST_ASSERT_EQUAL_CHAR('~', entry->path[PARADOX_AUDIT_PATH_MAX - 2]);
    TEST_ASSERT_EQUAL_CHAR('\0', entry->path[PARADOX_AUDIT_PATH_MAX - 1]);
    /* And the record after path in memory must be intact. This is the assert that
     * catches a truncation that writes one byte past its own field: an early
     * version of the writer spent the terminator slot on the mark, so a long path
     * ran into the status field and read as 67 characters. */
    TEST_ASSERT_EQUAL_STRING("200", entry->status);
    /* The method field is the narrow one: eight bytes is already generous for
     * GET/PUT/POST/DELETE, so anything longer is someone being creative. */
    TEST_ASSERT_EQUAL_STRING("PUT", entry->method);
}

/* A NUL inside a C string is where the field ends, not a byte to keep: that is
 * the C contract, and the record must follow it rather than invent a length.
 * Anything else that is not printable ASCII is replaced with a mark, so a control
 * byte or a raw high byte cannot carry a meaning of its own into a log line or a
 * terminal. */
static void test_unprintable_bytes_leave_a_mark(void)
{
    paradox_audit_record(1, "ad\0min", "GET", "/api/\r\nX-foo: 1\x1b[31m", "200",
                         "note with \xc3\xa9 accent");
    const paradox_audit_entry_t *entry = newest();
    TEST_ASSERT_EQUAL_STRING("ad", entry->principal);
    TEST_ASSERT_NOT_NULL(strstr(entry->path, "/api/..X-foo: 1.[31m"));
    TEST_ASSERT_NULL(strstr(entry->path, "\r"));
    TEST_ASSERT_NULL(strstr(entry->path, "\x1b"));
    TEST_ASSERT_NOT_NULL(strstr(entry->note, "note with .. accent"));
    TEST_ASSERT_EQUAL_STRING("GET", entry->method);
}

/* A client that puts a quote in a header value must not be able to end a field
 * early and invent the next one. */
static void test_quote_in_a_field_cannot_forge_a_record(void)
{
    paradox_audit_record(5, "anonymous", "POST",
                         "/api/zones/9\", \"principal\": \"admin", "403",
                         "confirm\":\"yes");
    TEST_ASSERT_EQUAL_SIZE(1u, paradox_audit_count());
    const paradox_audit_entry_t *entry = newest();
    TEST_ASSERT_EQUAL_STRING("anonymous", entry->principal);

    char json[1024];
    size_t length = paradox_audit_json(json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_SIZE(0u, length);
    /* Exactly one record object in the document, and the path's quotes came out
     * escaped rather than closing the string. */
    size_t objects = 0;
    for (const char *p = json; (p = strstr(p, "{\"sequence\":")) != NULL; ++p) {
        ++objects;
    }
    TEST_ASSERT_EQUAL_SIZE(1u, objects);
    TEST_ASSERT_NOT_NULL(strstr(json, "\\\"principal\\\": \\\"admin"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"principal\":\"anonymous\""));
}

/* The renderer reports failure instead of a half-written document, so the web
 * layer can answer with an error rather than JSON that stops mid-field. */
static void test_too_small_a_buffer_reports_failure_not_truncation(void)
{
    for (unsigned i = 0; i < 8u; ++i) {
        paradox_audit_record(1000 + i, "admin", "POST", "/api/ota", "200",
                             "image accepted, rebooting");
    }
    char big[4096];
    size_t needed = paradox_audit_json(big, sizeof(big));
    TEST_ASSERT_GREATER_THAN_SIZE(0u, needed);

    char small[32];
    strcpy(small, "sentinel");
    TEST_ASSERT_EQUAL_SIZE(0u, paradox_audit_json(small, sizeof(small)));
    TEST_ASSERT_EQUAL_STRING("", small);

    /* One byte short of what it needs is a failure too - the boundary is real -
     * and it has to be tested against a buffer that is exactly that size, because
     * a smaller stack buffer would make the test itself the overflow. */
    char *tight = malloc(needed - 1u);
    TEST_ASSERT_NOT_NULL(tight);
    TEST_ASSERT_EQUAL_SIZE(0u, paradox_audit_json(tight, needed - 1u));
    TEST_ASSERT_EQUAL_STRING("", tight);
    free(tight);
    /* And exactly enough works - the contract is one byte more than the length
     * that comes back, because the terminator has to live somewhere. */
    char room[4096];
    TEST_ASSERT_EQUAL_SIZE(needed, paradox_audit_json(room, needed + 1u));
    TEST_ASSERT_EQUAL_CHAR('}', room[needed - 1]);
    TEST_ASSERT_EQUAL_CHAR('\0', room[needed]);
    /* A buffer smaller than a header is refused without even trying. */
    TEST_ASSERT_EQUAL_SIZE(0u, paradox_audit_json(NULL, 1024));
}

/* The document has to be countable by a client without a JSON parser in the
 * firmware, and the capacity has to be stated so "16 records" is not mistaken
 * for "nothing older than that ever happened". */
static void test_document_names_its_own_capacity(void)
{
    paradox_audit_record(1, "operator", "POST", "/api/panel/arm", "403",
                         "network control is off");
    char json[512];
    size_t length = paradox_audit_json(json, sizeof(json));
    TEST_ASSERT_GREATER_THAN_SIZE(0u, length);
    TEST_ASSERT_EQUAL_CHAR('}', json[length - 1]);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"capacity\":16"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"next_sequence\":2"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"note\":\"network control is off\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"uptime_ms\":1"));
}

/* Newest-first ordering is what a person reads, so it has to be the contract
 * rather than an accident of the ring. */
static void test_snapshot_orders_newest_first(void)
{
    paradox_audit_record(1, "admin", "POST", "/one", "200", "");
    paradox_audit_record(2, "admin", "POST", "/two", "200", "");
    paradox_audit_record(3, "admin", "POST", "/three", "200", "");

    paradox_audit_entry_t three[3];
    TEST_ASSERT_EQUAL_SIZE(3u, paradox_audit_snapshot(three, 3));
    TEST_ASSERT_EQUAL_STRING("/three", three[0].path);
    TEST_ASSERT_EQUAL_STRING("/two", three[1].path);
    TEST_ASSERT_EQUAL_STRING("/one", three[2].path);
    /* Asking for fewer records than exist must still start at the newest. */
    paradox_audit_entry_t one;
    TEST_ASSERT_EQUAL_SIZE(1u, paradox_audit_snapshot(&one, 1));
    TEST_ASSERT_EQUAL_STRING("/three", one.path);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reset_leaves_an_empty_trail);
    RUN_TEST(test_record_keeps_the_fields_apart);
    RUN_TEST(test_refusals_are_records_too);
    RUN_TEST(test_null_fields_still_record);
    RUN_TEST(test_ring_is_bounded_and_overwrites_oldest);
    RUN_TEST(test_loss_is_visible_to_a_reader);
    RUN_TEST(test_long_fields_are_marked_as_truncated);
    RUN_TEST(test_unprintable_bytes_leave_a_mark);
    RUN_TEST(test_quote_in_a_field_cannot_forge_a_record);
    RUN_TEST(test_too_small_a_buffer_reports_failure_not_truncation);
    RUN_TEST(test_document_names_its_own_capacity);
    RUN_TEST(test_snapshot_orders_newest_first);
    return UNITY_END();
}
