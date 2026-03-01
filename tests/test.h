/* Test framework for emil */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* Test state */
static int _tests_run = 0;
static int _tests_failed = 0;
static int _current_test_failed = 0;
static const char *_current_test_name = NULL;

/* Assertion macros */
#define TEST_ASSERT(condition) do { \
    if (!(condition)) { \
        printf("  FAIL: %s:%d: Expression '%s' is false\n", \
               __FILE__, __LINE__, #condition); \
        _current_test_failed = 1; \
    } \
} while(0)

#define TEST_ASSERT_TRUE(condition) TEST_ASSERT(condition)
#define TEST_ASSERT_FALSE(condition) TEST_ASSERT(!(condition))
#define TEST_ASSERT_NULL(pointer) TEST_ASSERT((pointer) == NULL)
#define TEST_ASSERT_NOT_NULL(pointer) TEST_ASSERT((pointer) != NULL)

#define TEST_ASSERT_EQUAL(expected, actual) do { \
    if ((expected) != (actual)) { \
        printf("  FAIL: %s:%d: Expected %d but was %d\n", \
               __FILE__, __LINE__, (int)(expected), (int)(actual)); \
        _current_test_failed = 1; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_STRING(expected, actual) do { \
    if (strcmp((expected), (actual)) != 0) { \
        printf("  FAIL: %s:%d: Expected \"%s\" but was \"%s\"\n", \
               __FILE__, __LINE__, (expected), (actual)); \
        _current_test_failed = 1; \
    } \
} while(0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) TEST_ASSERT_EQUAL(expected, actual)
#define TEST_ASSERT_EQUAL_UINT(expected, actual) TEST_ASSERT_EQUAL(expected, actual)

/* Test runner macros */
#define TEST_BEGIN() do { \
    _tests_run = 0; \
    _tests_failed = 0; \
} while(0)

#define RUN_TEST(func) do { \
    _current_test_failed = 0; \
    _current_test_name = #func; \
    setUp(); \
    func(); \
    tearDown(); \
    _tests_run++; \
    if (_current_test_failed) { \
        _tests_failed++; \
        printf("  %s: FAIL\n", #func); \
    } \
} while(0)

#define TEST_END() ( \
    printf("%d Tests %d Failures\n", \
           _tests_run, _tests_failed), \
    (_tests_failed == 0) ? printf("OK\n") : printf("FAIL\n"), \
    _tests_failed \
)

/* setUp/tearDown — implemented by each test file */
void setUp(void);
void tearDown(void);

#endif /* TEST_H */
