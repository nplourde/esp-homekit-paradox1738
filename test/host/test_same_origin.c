/*
 * Host-side tests for the web server's cross-site request guard.
 *
 * There is no CSRF token on this device. The only thing standing between a
 * random page in the user's browser and a write to the alarm panel's
 * configuration is that a mutating request has to name this device in its
 * Origin (or Referer) header, and the header has to match the Host the request
 * was addressed to. That rule lives in paradox_same_origin.c so it can be tested
 * without esp_http_server; these are the cases that decide whether the guard is
 * a guard or a formality.
 */

#include "paradox_same_origin.h"

#include <stdio.h>
#include <string.h>

#include "unity.h"

static bool accept(const char *origin, const char *host)
{
    return paradox_same_origin_url(origin, strlen(origin), host);
}

void setUp(void) {}
void tearDown(void) {}

/* The dashboard and the API share one origin, so its own requests pass. */
static void test_exact_origin_is_accepted(void)
{
    TEST_ASSERT_TRUE(accept("http://192.168.4.30", "192.168.4.30"));
    TEST_ASSERT_TRUE(accept("http://esp1738.local", "esp1738.local"));
    /* Browsers append a path, query or fragment to the Referer form. */
    TEST_ASSERT_TRUE(accept("http://192.168.4.30/", "192.168.4.30"));
    TEST_ASSERT_TRUE(accept("http://192.168.4.30/zones.html", "192.168.4.30"));
    TEST_ASSERT_TRUE(accept("http://192.168.4.30/?tab=status", "192.168.4.30"));
    TEST_ASSERT_TRUE(accept("http://192.168.4.30#top", "192.168.4.30"));
}

/* A port is part of the origin: it has to appear on both sides, and match. */
static void test_port_is_part_of_the_origin(void)
{
    TEST_ASSERT_TRUE(accept("http://192.168.4.30:8088", "192.168.4.30:8088"));
    TEST_ASSERT_FALSE(accept("http://192.168.4.30:8088", "192.168.4.30:9000"));
    TEST_ASSERT_FALSE(accept("http://192.168.4.30", "192.168.4.30:8088"));
    TEST_ASSERT_FALSE(accept("http://192.168.4.30:8088", "192.168.4.30"));
}

/*
 * The dangerous direction: a page whose host merely starts with the device's
 * address. Without the check on the character after the host,
 * "http://192.168.4.30.evil.example" would be treated as the device itself.
 */
static void test_longer_host_is_not_the_same_origin(void)
{
    TEST_ASSERT_FALSE(accept("http://192.168.4.30.evil.example", "192.168.4.30"));
    TEST_ASSERT_FALSE(accept("http://192.168.4.30evil.example", "192.168.4.30"));
    TEST_ASSERT_FALSE(accept("http://192.168.4.300", "192.168.4.30"));
}

/* Neither a different scheme nor a different host counts. */
static void test_other_scheme_or_host_is_rejected(void)
{
    TEST_ASSERT_FALSE(accept("https://192.168.4.30", "192.168.4.30"));
    TEST_ASSERT_FALSE(accept("192.168.4.30", "192.168.4.30"));
    TEST_ASSERT_FALSE(accept("http://evil.example", "192.168.4.30"));
    TEST_ASSERT_FALSE(accept("http://192.168.4.31", "192.168.4.30"));
    /* "null" is what a sandboxed or data: page sends. */
    TEST_ASSERT_FALSE(accept("null", "192.168.4.30"));
}

/* A shorter Origin cannot name the whole Host. */
static void test_shorter_origin_is_rejected(void)
{
    TEST_ASSERT_FALSE(accept("http://192.168.4.3", "192.168.4.30"));
    TEST_ASSERT_FALSE(accept("http://", ""));
    TEST_ASSERT_FALSE(accept("http://192.168.4.30", "192.168.4.30.example"));
}

/*
 * Header values arrive with whatever length the caller measured, so a truncated
 * Origin must not be compared as if it were complete, and an empty one is not a
 * same-origin claim at all.
 */
static void test_lengths_are_trusted_over_nul_termination(void)
{
    const char truncated[] = "http://192.168.4.30/secret";
    /* The first 19 bytes are exactly "http://" plus the whole host. */
    TEST_ASSERT_TRUE(paradox_same_origin_url(truncated, 19, "192.168.4.30"));
    /* Cut one byte short of the host and it is no longer the device. */
    TEST_ASSERT_FALSE(paradox_same_origin_url(truncated, 18, "192.168.4.30"));
    TEST_ASSERT_FALSE(paradox_same_origin_url(truncated, 0, "192.168.4.30"));
    TEST_ASSERT_FALSE(paradox_same_origin_url(NULL, 24, "192.168.4.30"));
    TEST_ASSERT_FALSE(paradox_same_origin_url("http://192.168.4.30", 19, NULL));
    TEST_ASSERT_FALSE(paradox_same_origin_url("http://192.168.4.30", 19, ""));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exact_origin_is_accepted);
    RUN_TEST(test_port_is_part_of_the_origin);
    RUN_TEST(test_longer_host_is_not_the_same_origin);
    RUN_TEST(test_other_scheme_or_host_is_rejected);
    RUN_TEST(test_shorter_origin_is_rejected);
    RUN_TEST(test_lengths_are_trusted_over_nul_termination);
    return UNITY_END();
}
