/*
 * T0.1 - proves the host test harness works: Unity compiles and runs on the PC,
 * prints a summary, and its exit status reflects pass/fail.
 *
 * The failure path is exercised out-of-band by `make check-fail`, which builds
 * samples/deliberate_failure.c instead, so this suite stays green.
 */
#include <unity.h>

static int g_setups;
static int g_teardowns;

void setUp(void)
{
    g_setups++;
}

void tearDown(void)
{
    g_teardowns++;
}

static void test_setup_hook_ran_before_test(void)
{
    TEST_ASSERT_EQUAL_INT(1, g_setups);
    TEST_ASSERT_EQUAL_INT(0, g_teardowns);
}

static void test_unity_assertions_pass(void)
{
    const unsigned char frame[4] = {0x04, 0x10, 0x34, 0x56};
    TEST_ASSERT_EQUAL_INT(4, (int)sizeof(frame));
    TEST_ASSERT_EQUAL_HEX8(0x04, frame[0]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_setup_hook_ran_before_test);
    RUN_TEST(test_unity_assertions_pass);
    return UNITY_END();
}
