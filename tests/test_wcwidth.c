/* test_wcwidth.c — Character and string width calculations. */

#include "test.h"
#include "test_harness.h"
#include "unicode.h"
#include "wcwidth.h"
#include <stdint.h>

void test_char_width(void) {
	TEST_ASSERT_EQUAL_INT(1, mk_wcwidth('A'));
	TEST_ASSERT_EQUAL_INT(1, mk_wcwidth(' '));
	TEST_ASSERT_EQUAL_INT(-1, mk_wcwidth('\t'));
	TEST_ASSERT_EQUAL_INT(-1, mk_wcwidth('\n'));
	TEST_ASSERT(mk_wcwidth('\0') <= 0);
}

void test_string_width(void) {
	TEST_ASSERT_EQUAL_INT(5, stringWidth((uint8_t *)"Hello"));
	TEST_ASSERT_EQUAL_INT(0, stringWidth((uint8_t *)""));
	TEST_ASSERT_EQUAL_INT(11, stringWidth((uint8_t *)"Hello World"));
}

void test_char_in_string_width(void) {
	uint8_t ctrl[] = "\x01\x02\x0F";
	TEST_ASSERT_EQUAL_INT(2, charInStringWidth(ctrl, 0));
	TEST_ASSERT_EQUAL_INT(2, charInStringWidth(ctrl, 1));
	TEST_ASSERT_EQUAL_INT(2, charInStringWidth(ctrl, 2));

	uint8_t ascii[] = "ABC";
	TEST_ASSERT_EQUAL_INT(1, charInStringWidth(ascii, 0));

	uint8_t del[] = "\x7F";
	TEST_ASSERT_EQUAL_INT(2, charInStringWidth(del, 0));
}

void test_next_screen_x(void) {
	int idx;

	uint8_t tab[] = "\t";
	idx = 0;
	TEST_ASSERT_EQUAL_INT(8, nextScreenX(tab, &idx, 0));
	idx = 0;
	TEST_ASSERT_EQUAL_INT(8, nextScreenX(tab, &idx, 5));
	idx = 0;
	TEST_ASSERT_EQUAL_INT(16, nextScreenX(tab, &idx, 8));

	uint8_t a[] = "A";
	idx = 0;
	TEST_ASSERT_EQUAL_INT(1, nextScreenX(a, &idx, 0));
	idx = 0;
	TEST_ASSERT_EQUAL_INT(6, nextScreenX(a, &idx, 5));

	uint8_t ctrl[] = "\x01";
	idx = 0;
	TEST_ASSERT_EQUAL_INT(2, nextScreenX(ctrl, &idx, 0));
}

void test_tab_stops(void) {
#define TAB_STOP 8
	TEST_ASSERT_EQUAL_INT(8, (0 + TAB_STOP) / TAB_STOP * TAB_STOP);
	TEST_ASSERT_EQUAL_INT(8, (7 + TAB_STOP) / TAB_STOP * TAB_STOP);
	TEST_ASSERT_EQUAL_INT(16, (8 + TAB_STOP) / TAB_STOP * TAB_STOP);
	TEST_ASSERT_EQUAL_INT(16, (9 + TAB_STOP) / TAB_STOP * TAB_STOP);
#undef TAB_STOP
}

void test_cjk_double_width(void) {
	TEST_ASSERT_EQUAL_INT(2, mk_wcwidth(0x4E00));
	TEST_ASSERT_EQUAL_INT(2, mk_wcwidth(0x9FFF));
	TEST_ASSERT_EQUAL_INT(2, mk_wcwidth(0x3000));
}

void test_zero_width_combining(void) {
	TEST_ASSERT_EQUAL_INT(0, mk_wcwidth(0x0300));
	TEST_ASSERT_EQUAL_INT(0, mk_wcwidth(0x0301));
	TEST_ASSERT_EQUAL_INT(0, mk_wcwidth(0x20DD));
}

void test_next_screen_x_multibyte(void) {
	int idx;

	uint8_t cent[] = "\xC2\xA2";
	idx = 0;
	int result = nextScreenX(cent, &idx, 0);
	TEST_ASSERT_EQUAL_INT(1, result);
	TEST_ASSERT_EQUAL_INT(1, idx);

	uint8_t cjk[] = "\xE4\xB8\x80";
	idx = 0;
	result = nextScreenX(cjk, &idx, 0);
	TEST_ASSERT_EQUAL_INT(2, result);
	TEST_ASSERT_EQUAL_INT(2, idx);
}

void test_string_width_mixed(void) {
	/* "A一B" = 1 + 2 + 1 = 4 columns */
	uint8_t mixed[] = "A\xE4\xB8\x80" "B";
	TEST_ASSERT_EQUAL_INT(4, stringWidth(mixed));
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_char_width);
	RUN_TEST(test_string_width);
	RUN_TEST(test_char_in_string_width);
	RUN_TEST(test_next_screen_x);
	RUN_TEST(test_tab_stops);
	RUN_TEST(test_cjk_double_width);
	RUN_TEST(test_zero_width_combining);
	RUN_TEST(test_next_screen_x_multibyte);
	RUN_TEST(test_string_width_mixed);
	return TEST_END();
}
