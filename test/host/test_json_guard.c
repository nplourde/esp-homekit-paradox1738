#include <unity.h>

#include "paradox_json_guard.h"

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static size_t deepest(const char *text, size_t max_depth, bool *ok_out)
{
    size_t depth = 999;
    const bool ok = paradox_json_depth_ok(text, strlen(text), max_depth, &depth);
    if (ok_out != NULL) {
        *ok_out = ok;
    }
    return depth;
}

/* A normal API body is a flat object, and the guard must be invisible to it. */
void test_flat_object_is_accepted(void)
{
    bool ok = false;
    TEST_ASSERT_EQUAL_UINT(1, deepest("{\"name\":\"Cuisine\",\"enabled\":true}",
                                      PARADOX_JSON_MAX_DEPTH, &ok));
    TEST_ASSERT_TRUE(ok);

    /* No containers at all, and an empty body, are both within any limit. */
    TEST_ASSERT_EQUAL_UINT(0, deepest("", PARADOX_JSON_MAX_DEPTH, &ok));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT(0, deepest("7", PARADOX_JSON_MAX_DEPTH, &ok));
    TEST_ASSERT_TRUE(ok);
}

/* The nesting limit is what keeps cJSON recursion inside the web task stack, so
 * the exact boundary matters: max_depth levels are allowed, one more is not.
 * nest() builds n open brackets followed by n closers. */
static size_t nest(char *out, size_t capacity, size_t n)
{
    size_t used = 0;
    while (used < n) {
        out[used++] = '[';
    }
    while (used < 2 * n && used < capacity - 1u) {
        out[used++] = ']';
    }
    out[used] = '\0';
    return used;
}

void test_depth_limit_is_exact(void)
{
    char body[512];
    size_t depth = 0;

    /* Exactly max_depth containers deep is accepted. */
    size_t used = nest(body, sizeof(body), PARADOX_JSON_MAX_DEPTH);
    TEST_ASSERT_TRUE(paradox_json_depth_ok(body, used, PARADOX_JSON_MAX_DEPTH,
                                           &depth));
    TEST_ASSERT_EQUAL_UINT(PARADOX_JSON_MAX_DEPTH, depth);

    /* One level more is refused, and the reported depth stops at the breach
     * rather than counting on through a hostile body. */
    used = nest(body, sizeof(body), PARADOX_JSON_MAX_DEPTH + 1u);
    TEST_ASSERT_FALSE(paradox_json_depth_ok(body, used, PARADOX_JSON_MAX_DEPTH,
                                            &depth));
    TEST_ASSERT_EQUAL_UINT(PARADOX_JSON_MAX_DEPTH + 1u, depth);

    /* The 200-level array that used to reboot the module is refused the same way,
     * with the counter never climbing past the breach. */
    char deep[401];
    used = nest(deep, sizeof(deep), 200);
    TEST_ASSERT_EQUAL_UINT(400u, used);
    TEST_ASSERT_FALSE(paradox_json_depth_ok(deep, used, PARADOX_JSON_MAX_DEPTH,
                                            &depth));
    TEST_ASSERT_EQUAL_UINT(PARADOX_JSON_MAX_DEPTH + 1u, depth);

    /* A limit of zero allows nothing that opens a container. */
    TEST_ASSERT_FALSE(paradox_json_depth_ok("{}", 2, 0, &depth));
    TEST_ASSERT_EQUAL_UINT(1, depth);
}

/* Brackets inside strings are data: a zone name may contain them, and the depth
 * count must follow the parser rather than the characters. */
void test_brackets_inside_strings_are_not_counted(void)
{
    bool ok = false;
    TEST_ASSERT_EQUAL_UINT(1, deepest("{\"name\":\"[[[[[[[[[[[[[[[[\"}",
                                      PARADOX_JSON_MAX_DEPTH, &ok));
    TEST_ASSERT_TRUE(ok);

    /* An escaped quote does not end the string, so the bracket after it is still
     * inside the name. */
    TEST_ASSERT_EQUAL_UINT(1, deepest("{\"name\":\"a\\\"[[[[[[[[[[\"}",
                                      PARADOX_JSON_MAX_DEPTH, &ok));
    TEST_ASSERT_TRUE(ok);

    /* A literal backslash (written \\) does end the string on the next quote, so
     * the brackets that follow really are structure and do count. */
    TEST_ASSERT_EQUAL_UINT(2, deepest("{\"n\":\"\\\\\"}[[",
                                      PARADOX_JSON_MAX_DEPTH, &ok));
    TEST_ASSERT_TRUE(ok);
}

/* The guard bounds depth; it does not validate JSON. Anything the parser is going
 * to reject anyway must not look like a depth problem here, so the caller's error
 * message stays the accurate one. */
void test_malformed_input_is_not_a_depth_problem(void)
{
    bool ok = false;

    /* Truncated body: the parser says "Invalid JSON object". */
    TEST_ASSERT_EQUAL_UINT(1, deepest("{\"name\":\"x", PARADOX_JSON_MAX_DEPTH,
                                      &ok));
    TEST_ASSERT_TRUE(ok);
    TEST_ASSERT_EQUAL_UINT(1, deepest("{\"name\":\"x\"", PARADOX_JSON_MAX_DEPTH,
                                      &ok));
    TEST_ASSERT_TRUE(ok);

    /* More closers than openers must not drive the counter negative. */
    TEST_ASSERT_EQUAL_UINT(1, deepest("]]]]{\"a\":1}", PARADOX_JSON_MAX_DEPTH,
                            &ok));
    TEST_ASSERT_TRUE(ok);

    /* Text that is not JSON at all. */
    TEST_ASSERT_EQUAL_UINT(0, deepest("not json at all", PARADOX_JSON_MAX_DEPTH,
                                      &ok));
    TEST_ASSERT_TRUE(ok);
}

void test_null_text_is_rejected(void)
{
    size_t depth = 999;
    TEST_ASSERT_FALSE(paradox_json_depth_ok(NULL, 0, PARADOX_JSON_MAX_DEPTH,
                                            &depth));
    TEST_ASSERT_EQUAL_UINT(0, depth);
    /* The depth output is optional. */
    TEST_ASSERT_TRUE(paradox_json_depth_ok("{}", 2, PARADOX_JSON_MAX_DEPTH, NULL));
}

/* Depth is counted per body, so a rejected body must not leave state behind that
 * makes a later, legitimate body fail. */
void test_state_does_not_leak_between_bodies(void)
{
    bool ok = false;
    TEST_ASSERT_FALSE(paradox_json_depth_ok("[[[[[[[[[[", 10,
                                            PARADOX_JSON_MAX_DEPTH, NULL));
    TEST_ASSERT_EQUAL_UINT(1, deepest("{\"name\":\"Front door\"}", 8, &ok));
    TEST_ASSERT_TRUE(ok);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_flat_object_is_accepted);
    RUN_TEST(test_depth_limit_is_exact);
    RUN_TEST(test_brackets_inside_strings_are_not_counted);
    RUN_TEST(test_malformed_input_is_not_a_depth_problem);
    RUN_TEST(test_null_text_is_rejected);
    RUN_TEST(test_state_does_not_leak_between_bodies);
    return UNITY_END();
}
