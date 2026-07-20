/* Test framework for emil */
#ifndef TEST_H
#define TEST_H

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

/* Test state */
static int _tests_run = 0;
static int _tests_failed = 0;
static int _current_test_failed = 0;
static const char *_current_test_name = NULL;

/* Name of the test currently executing, for failure messages.  Only
 * assertions inside RUN_TEST() are counted toward the failure total;
 * the NULL guard exists solely so printf always gets a valid string. */
#define _TEST_NAME() (_current_test_name ? _current_test_name : "<no test>")

/* Assertion macros.
 *
 * Every macro evaluates each of its arguments EXACTLY ONCE, into a local
 * temporary.  This matters: assertions here routinely wrap function calls
 * with side effects, and re-evaluating an argument to build the failure
 * message would both repeat the side effect and risk reporting a value
 * that was never the one compared. */

#define TEST_ASSERT(condition)                                                   \
	do {                                                                     \
		if (!(condition)) {                                              \
			printf("  FAIL: %s:%d [%s]: Expression '%s' is false\n", \
			       __FILE__, __LINE__, _TEST_NAME(), #condition);    \
			_current_test_failed = 1;                                \
		}                                                                \
	} while (0)

#define TEST_ASSERT_TRUE(condition) TEST_ASSERT(condition)
#define TEST_ASSERT_FALSE(condition) TEST_ASSERT(!(condition))
#define TEST_ASSERT_NULL(pointer) TEST_ASSERT((pointer) == NULL)
#define TEST_ASSERT_NOT_NULL(pointer) TEST_ASSERT((pointer) != NULL)

/* Signed integer comparison.  Values are widened to intmax_t rather than
 * truncated to int, so that size_t/off_t/long operands compare and print
 * correctly instead of silently dropping their high bits. */
#define TEST_ASSERT_EQUAL(expected, actual)                                      \
	do {                                                                     \
		intmax_t _t_exp = (intmax_t)(expected);                          \
		intmax_t _t_act = (intmax_t)(actual);                            \
		if (_t_exp != _t_act) {                                          \
			printf("  FAIL: %s:%d [%s]: Expected %jd but was %jd\n", \
			       __FILE__, __LINE__, _TEST_NAME(), _t_exp,         \
			       _t_act);                                          \
			_current_test_failed = 1;                                \
		}                                                                \
	} while (0)

/* Unsigned comparison.  Kept distinct from the signed form so that large
 * unsigned values are not reported as negative numbers. */
#define TEST_ASSERT_EQUAL_UINT(expected, actual)                                 \
	do {                                                                     \
		uintmax_t _t_exp = (uintmax_t)(expected);                        \
		uintmax_t _t_act = (uintmax_t)(actual);                          \
		if (_t_exp != _t_act) {                                          \
			printf("  FAIL: %s:%d [%s]: Expected %ju but was %ju\n", \
			       __FILE__, __LINE__, _TEST_NAME(), _t_exp,         \
			       _t_act);                                          \
			_current_test_failed = 1;                                \
		}                                                                \
	} while (0)

/* String comparison.  NULL on either side is reported as a normal test
 * failure instead of dereferencing into a crash; two NULLs compare equal. */
#define TEST_ASSERT_EQUAL_STRING(expected, actual)                                             \
	do {                                                                                   \
		const char *_t_exp = (expected);                                               \
		const char *_t_act = (actual);                                                 \
		if (_t_exp == NULL || _t_act == NULL) {                                        \
			if (_t_exp != _t_act) {                                                \
				printf("  FAIL: %s:%d [%s]: Expected \"%s\" but was \"%s\"\n", \
				       __FILE__, __LINE__, _TEST_NAME(),                       \
				       _t_exp ? _t_exp : "(null)",                             \
				       _t_act ? _t_act : "(null)");                            \
				_current_test_failed = 1;                                      \
			}                                                                      \
		} else if (strcmp(_t_exp, _t_act) != 0) {                                      \
			printf("  FAIL: %s:%d [%s]: Expected \"%s\" but was \"%s\"\n",         \
			       __FILE__, __LINE__, _TEST_NAME(), _t_exp,                       \
			       _t_act);                                                        \
			_current_test_failed = 1;                                              \
		}                                                                              \
	} while (0)

#define TEST_ASSERT_EQUAL_INT(expected, actual) \
	TEST_ASSERT_EQUAL(expected, actual)

/* Test runner macros */
#define TEST_BEGIN()               \
	do {                       \
		_tests_run = 0;    \
		_tests_failed = 0; \
	} while (0)

#define RUN_TEST(func)                                 \
	do {                                           \
		_current_test_failed = 0;              \
		_current_test_name = #func;            \
		setUp();                               \
		func();                                \
		tearDown();                            \
		_tests_run++;                          \
		if (_current_test_failed) {            \
			_tests_failed++;               \
			printf("  %s: FAIL\n", #func); \
		}                                      \
		_current_test_name = NULL;             \
	} while (0)

/* Yields a process exit status: 0 on success, 1 on any failure.  The raw
 * failure count is deliberately NOT returned, since an exit status is
 * truncated to 8 bits and exactly 256 failures would be reported as
 * success. */
#define TEST_END()                                                    \
	(printf("%d Tests %d Failures\n", _tests_run, _tests_failed), \
	 (_tests_failed == 0) ? printf("OK\n") : printf("FAIL\n"),    \
	 (_tests_failed != 0))

/* setUp/tearDown — implemented by each test file */
void setUp(void);
void tearDown(void);

#endif /* TEST_H */
