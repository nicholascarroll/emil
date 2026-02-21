/* test_unicode.c — UTF-8 classification and validation. */

#include "test.h"
#include "test_harness.h"
#include "unicode.h"
#include <stdint.h>

void test_utf8_bytes(void) {
	TEST_ASSERT_EQUAL_INT(1, utf8_nBytes('A'));
	TEST_ASSERT_EQUAL_INT(1, utf8_nBytes('0'));
	TEST_ASSERT_EQUAL_INT(2, utf8_nBytes(0xC2));
	TEST_ASSERT_EQUAL_INT(3, utf8_nBytes(0xE0));
	TEST_ASSERT_EQUAL_INT(4, utf8_nBytes(0xF0));
}

void test_utf8_continuation(void) {
	TEST_ASSERT_TRUE(utf8_isCont(0x80));
	TEST_ASSERT_TRUE(utf8_isCont(0xBF));
	TEST_ASSERT_FALSE(utf8_isCont('A'));
	TEST_ASSERT_FALSE(utf8_isCont(0xC0));
}

void test_utf8_char_types(void) {
	/* 2-byte start bytes */
	TEST_ASSERT_TRUE(utf8_is2Char(0xC2));
	TEST_ASSERT_TRUE(utf8_is2Char(0xDF));
	TEST_ASSERT_FALSE(utf8_is2Char(0xC1));
	TEST_ASSERT_FALSE(utf8_is2Char(0xE0));

	/* 3-byte start bytes */
	TEST_ASSERT_TRUE(utf8_is3Char(0xE0));
	TEST_ASSERT_TRUE(utf8_is3Char(0xEF));
	TEST_ASSERT_FALSE(utf8_is3Char(0xDF));
	TEST_ASSERT_FALSE(utf8_is3Char(0xF0));

	/* 4-byte start bytes */
	TEST_ASSERT_TRUE(utf8_is4Char(0xF0));
	TEST_ASSERT_TRUE(utf8_is4Char(0xF4));
	TEST_ASSERT_FALSE(utf8_is4Char(0xF5));
	TEST_ASSERT_FALSE(utf8_is4Char(0xEF));
}

void test_utf8_validation_sequences(void) {
	/* Valid 2-byte: ¢ */
	uint8_t v2[] = "\xC2\xA2";
	TEST_ASSERT_TRUE(utf8_is2Char(v2[0]));
	TEST_ASSERT_TRUE(utf8_isCont(v2[1]));

	/* Valid 3-byte: € */
	uint8_t v3[] = "\xE2\x82\xAC";
	TEST_ASSERT_TRUE(utf8_is3Char(v3[0]));
	TEST_ASSERT_TRUE(utf8_isCont(v3[1]));
	TEST_ASSERT_TRUE(utf8_isCont(v3[2]));

	/* Valid 4-byte: 😀 */
	uint8_t v4[] = "\xF0\x9F\x98\x80";
	TEST_ASSERT_TRUE(utf8_is4Char(v4[0]));
	TEST_ASSERT_TRUE(utf8_isCont(v4[1]));
	TEST_ASSERT_TRUE(utf8_isCont(v4[2]));
	TEST_ASSERT_TRUE(utf8_isCont(v4[3]));
}

void test_control_chars(void) {
	TEST_ASSERT_FALSE(ISCTRL('\0'));
	TEST_ASSERT_TRUE(ISCTRL('\n'));
	TEST_ASSERT_TRUE(ISCTRL('\r'));
	TEST_ASSERT_TRUE(ISCTRL('\t'));
	TEST_ASSERT_TRUE(ISCTRL(0x7f));
	TEST_ASSERT_FALSE(ISCTRL(' '));
	TEST_ASSERT_FALSE(ISCTRL('A'));
}

void test_overlong_encoding(void) {
	TEST_ASSERT_FALSE(utf8_is2Char(0xC0));
	TEST_ASSERT_FALSE(utf8_is2Char(0xC1));
}

void test_boundary_3byte_4byte(void) {
	TEST_ASSERT_TRUE(utf8_is3Char(0xEF));
	TEST_ASSERT_FALSE(utf8_is4Char(0xEF));
	TEST_ASSERT_TRUE(utf8_is4Char(0xF0));
	TEST_ASSERT_FALSE(utf8_is3Char(0xF0));
}

void test_invalid_lead_bytes(void) {
	TEST_ASSERT_FALSE(utf8_is4Char(0xF5));
	TEST_ASSERT_FALSE(utf8_is4Char(0xFE));
	TEST_ASSERT_FALSE(utf8_is4Char(0xFF));
	TEST_ASSERT_FALSE(utf8_is2Char(0xFE));
	TEST_ASSERT_FALSE(utf8_is3Char(0xFF));
}

void test_continuation_not_start(void) {
	TEST_ASSERT_FALSE(utf8_is2Char(0x80));
	TEST_ASSERT_FALSE(utf8_is3Char(0x80));
	TEST_ASSERT_FALSE(utf8_is4Char(0x80));
	TEST_ASSERT_TRUE(utf8_isCont(0x80));
	TEST_ASSERT_EQUAL_INT(1, utf8_nBytes(0x80));
	TEST_ASSERT_EQUAL_INT(1, utf8_nBytes(0xBF));
}

void test_nbytes_all_ranges(void) {
	TEST_ASSERT_EQUAL_INT(1, utf8_nBytes(0x00));
	TEST_ASSERT_EQUAL_INT(1, utf8_nBytes(0x7F));
	TEST_ASSERT_EQUAL_INT(2, utf8_nBytes(0xC2));
	TEST_ASSERT_EQUAL_INT(2, utf8_nBytes(0xDF));
	TEST_ASSERT_EQUAL_INT(3, utf8_nBytes(0xE0));
	TEST_ASSERT_EQUAL_INT(3, utf8_nBytes(0xEF));
	TEST_ASSERT_EQUAL_INT(4, utf8_nBytes(0xF0));
	TEST_ASSERT_EQUAL_INT(4, utf8_nBytes(0xF4));
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_utf8_bytes);
	RUN_TEST(test_utf8_continuation);
	RUN_TEST(test_utf8_char_types);
	RUN_TEST(test_utf8_validation_sequences);
	RUN_TEST(test_control_chars);
	RUN_TEST(test_overlong_encoding);
	RUN_TEST(test_boundary_3byte_4byte);
	RUN_TEST(test_invalid_lead_bytes);
	RUN_TEST(test_continuation_not_start);
	RUN_TEST(test_nbytes_all_ranges);
	return TEST_END();
}
