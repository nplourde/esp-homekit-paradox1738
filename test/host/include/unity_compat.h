/*
 * Compatibility shims for the Unity version vendored in ESP-IDF.
 *
 * ESP-IDF 5.5 ships Unity 2.6.0, which has no size_t asserts. On the host
 * size_t is 64-bit, so compare through the UINT64 family (Unity auto-enables
 * UNITY_SUPPORT_64 whenever long or pointer is 64-bit).
 */
#ifndef UNITY_COMPAT_H
#define UNITY_COMPAT_H

#include <stdint.h>

#include <unity.h>

#define TEST_ASSERT_EQUAL_SIZE(expected, actual) \
    TEST_ASSERT_EQUAL_UINT64((uint64_t)(expected), (uint64_t)(actual))

#define TEST_ASSERT_EQUAL_SIZE_MESSAGE(expected, actual, message) \
    TEST_ASSERT_EQUAL_UINT64_MESSAGE((uint64_t)(expected), (uint64_t)(actual), (message))

#endif /* UNITY_COMPAT_H */
