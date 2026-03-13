/* test_utf8_validate.c — Tests for the utf8_validate() utility. */

#include "test.h"
#include "test_harness.h"
#include "unicode.h"
#include <stdint.h>

/* --- Valid sequences --- */

void test_valid_ascii(void) {
	TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)"hello", 5));
	TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)"A", 1));
	TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)" ", 1));
	TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)"~", 1));
}

void test_valid_2byte(void) {
	/* ¢ = C2 A2 */
	uint8_t s[] = {0xC2, 0xA2};
	TEST_ASSERT_TRUE(utf8_validate(s, 2));
	/* ß = C3 9F */
	uint8_t s2[] = {0xC3, 0x9F};
	TEST_ASSERT_TRUE(utf8_validate(s2, 2));
}

void test_valid_3byte(void) {
	/* € = E2 82 AC */
	uint8_t s[] = {0xE2, 0x82, 0xAC};
	TEST_ASSERT_TRUE(utf8_validate(s, 3));
	/* 한 = ED 95 9C */
	uint8_t s2[] = {0xED, 0x95, 0x9C};
	TEST_ASSERT_TRUE(utf8_validate(s2, 3));
}

void test_valid_4byte(void) {
	/* 😇 = F0 9F 98 87 */
	uint8_t s[] = {0xF0, 0x9F, 0x98, 0x87};
	TEST_ASSERT_TRUE(utf8_validate(s, 4));
	/* 𐍈 = F0 90 8D 88 */
	uint8_t s2[] = {0xF0, 0x90, 0x8D, 0x88};
	TEST_ASSERT_TRUE(utf8_validate(s2, 4));
}

void test_valid_mixed(void) {
	/* "A¢€😇" */
	uint8_t s[] = {'A', 0xC2, 0xA2, 0xE2, 0x82, 0xAC,
		       0xF0, 0x9F, 0x98, 0x87};
	TEST_ASSERT_TRUE(utf8_validate(s, 10));
}

void test_valid_empty(void) {
	uint8_t s[] = {0};
	/* len=0 is trivially valid */
	TEST_ASSERT_TRUE(utf8_validate(s, 0));
}

/* --- Invalid: null byte --- */

void test_null_byte(void) {
	uint8_t s[] = {'A', 0x00, 'B'};
	TEST_ASSERT_FALSE(utf8_validate(s, 3));
}

/* --- Invalid: overlong encodings --- */

void test_overlong_2byte(void) {
	/* C0 80 = overlong NUL */
	uint8_t s[] = {0xC0, 0x80};
	TEST_ASSERT_FALSE(utf8_validate(s, 2));
	/* C1 BF = overlong U+007F */
	uint8_t s2[] = {0xC1, 0xBF};
	TEST_ASSERT_FALSE(utf8_validate(s2, 2));
}

void test_overlong_3byte(void) {
	/* E0 80 80 = overlong NUL */
	uint8_t s[] = {0xE0, 0x80, 0x80};
	TEST_ASSERT_FALSE(utf8_validate(s, 3));
	/* E0 9F BF = overlong U+07FF */
	uint8_t s2[] = {0xE0, 0x9F, 0xBF};
	TEST_ASSERT_FALSE(utf8_validate(s2, 3));
}

void test_overlong_4byte(void) {
	/* F0 80 80 80 = overlong NUL */
	uint8_t s[] = {0xF0, 0x80, 0x80, 0x80};
	TEST_ASSERT_FALSE(utf8_validate(s, 4));
	/* F0 8F BF BF = overlong U+FFFF */
	uint8_t s2[] = {0xF0, 0x8F, 0xBF, 0xBF};
	TEST_ASSERT_FALSE(utf8_validate(s2, 4));
}

/* --- Invalid: surrogates --- */

void test_surrogate_halves(void) {
	/* ED A0 80 = U+D800 (high surrogate) */
	uint8_t s[] = {0xED, 0xA0, 0x80};
	TEST_ASSERT_FALSE(utf8_validate(s, 3));
	/* ED BF BF = U+DFFF (low surrogate) */
	uint8_t s2[] = {0xED, 0xBF, 0xBF};
	TEST_ASSERT_FALSE(utf8_validate(s2, 3));
}

/* --- Invalid: above U+10FFFF --- */

void test_above_max(void) {
	/* F4 90 80 80 = U+110000 */
	uint8_t s[] = {0xF4, 0x90, 0x80, 0x80};
	TEST_ASSERT_FALSE(utf8_validate(s, 4));
	/* F5 80 80 80 = lead byte > F4 */
	uint8_t s2[] = {0xF5, 0x80, 0x80, 0x80};
	TEST_ASSERT_FALSE(utf8_validate(s2, 4));
}

/* --- Invalid: bad continuation bytes --- */

void test_bad_continuation(void) {
	/* 2-byte with bad cont */
	uint8_t s[] = {0xC2, 0x00};
	TEST_ASSERT_FALSE(utf8_validate(s, 2));
	uint8_t s2[] = {0xC2, 0xC0};
	TEST_ASSERT_FALSE(utf8_validate(s2, 2));
	/* 3-byte with bad 2nd cont */
	uint8_t s3[] = {0xE2, 0x82, 0x00};
	TEST_ASSERT_FALSE(utf8_validate(s3, 3));
}

/* --- Invalid: truncated sequences --- */

void test_truncated(void) {
	/* 2-byte truncated */
	uint8_t s[] = {0xC2};
	TEST_ASSERT_FALSE(utf8_validate(s, 1));
	/* 3-byte truncated after 2 */
	uint8_t s2[] = {0xE2, 0x82};
	TEST_ASSERT_FALSE(utf8_validate(s2, 2));
	/* 4-byte truncated after 3 */
	uint8_t s3[] = {0xF0, 0x9F, 0x98};
	TEST_ASSERT_FALSE(utf8_validate(s3, 3));
}

/* --- Invalid: bare continuation byte as lead --- */

void test_bare_continuation(void) {
	uint8_t s[] = {0x80};
	TEST_ASSERT_FALSE(utf8_validate(s, 1));
	uint8_t s2[] = {0xBF};
	TEST_ASSERT_FALSE(utf8_validate(s2, 1));
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_valid_ascii);
	RUN_TEST(test_valid_2byte);
	RUN_TEST(test_valid_3byte);
	RUN_TEST(test_valid_4byte);
	RUN_TEST(test_valid_mixed);
	RUN_TEST(test_valid_empty);
	RUN_TEST(test_null_byte);
	RUN_TEST(test_overlong_2byte);
	RUN_TEST(test_overlong_3byte);
	RUN_TEST(test_overlong_4byte);
	RUN_TEST(test_surrogate_halves);
	RUN_TEST(test_above_max);
	RUN_TEST(test_bad_continuation);
	RUN_TEST(test_truncated);
	RUN_TEST(test_bare_continuation);
	return TEST_END();
}
