/*
 * Not part of `make check`. `make check-fail` builds this to prove the harness
 * fails loudly: a red run must exit non-zero and name this file and line.
 */
#include <unity.h>

void setUp(void) {}
void tearDown(void) {}

static void test_this_must_fail(void)
{
    TEST_ASSERT_EQUAL_INT(1, 2);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_this_must_fail);
    return UNITY_END();
}
