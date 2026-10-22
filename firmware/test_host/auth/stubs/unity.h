/* Run the device's existing Unity cases on the host using libc assertions. */
#pragma once
#include <assert.h>
#include <stdio.h>
#include <string.h>
void host_register_test(const char *name, void (*fn)(void));
void unity_run_all_tests(void);
#define JOIN_(a,b) a##b
#define JOIN(a,b) JOIN_(a,b)
#define TEST_CASE(name, tags) \
    static void JOIN(test_, __LINE__)(void); \
    static void __attribute__((constructor)) JOIN(register_, __LINE__)(void) { \
        host_register_test(name, JOIN(test_, __LINE__)); \
    } \
    static void JOIN(test_, __LINE__)(void)
#define UNITY_BEGIN() ((void)0)
#define UNITY_END() ((void)0)
#define TEST_ASSERT_EQUAL(a,b) assert((a) == (b))
#define TEST_ASSERT_EQUAL_INT(a,b) TEST_ASSERT_EQUAL(a,b)
#define TEST_ASSERT_EQUAL_UINT32(a,b) TEST_ASSERT_EQUAL(a,b)
#define TEST_ASSERT_EQUAL_UINT64(a,b) TEST_ASSERT_EQUAL(a,b)
#define TEST_ASSERT_EQUAL_MESSAGE(a,b,m) TEST_ASSERT_EQUAL(a,b)
#define TEST_ASSERT_NOT_NULL(a) assert((a) != NULL)
#define TEST_ASSERT_NOT_NULL_MESSAGE(a,m) TEST_ASSERT_NOT_NULL(a)
#define TEST_ASSERT_EQUAL_STRING(a,b) assert(strcmp((a),(b)) == 0)
#define TEST_ASSERT_EQUAL_MEMORY(a,b,n) assert(memcmp((a),(b),(n)) == 0)
