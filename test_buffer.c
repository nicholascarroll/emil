/* test_buffer.c — Row operations, screen cache, coordinate mapping. */

#include "test.h"
#include "test_harness.h"
#include <stdint.h>

/* ---- Row operations ---- */

void test_new_destroy_buffer(void) {
	struct buffer *buf = newBuffer();
	TEST_ASSERT_NOT_NULL(buf);
	TEST_ASSERT_EQUAL_INT(0, buf->numrows);
	TEST_ASSERT_NULL(buf->filename);
	destroyBuffer(buf);
}

void test_insert_row_beginning(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, "second", 6);
	insertRow(buf, 0, "first", 5);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("first", (char *)buf->row[0].chars);
	TEST_ASSERT_EQUAL_STRING("second", (char *)buf->row[1].chars);
}

void test_insert_row_middle(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, "first", 5);
	insertRow(buf, 1, "third", 5);
	insertRow(buf, 1, "second", 6);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("second", (char *)buf->row[1].chars);
}

void test_insert_row_end(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, "first", 5);
	insertRow(buf, 1, "second", 6);
	insertRow(buf, 2, "third", 5);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("third", (char *)buf->row[2].chars);
}

void test_del_row_beginning(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, "first", 5);
	insertRow(buf, 1, "second", 6);
	insertRow(buf, 2, "third", 5);
	delRow(buf, 0);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("second", (char *)buf->row[0].chars);
}

void test_del_row_middle(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, "first", 5);
	insertRow(buf, 1, "second", 6);
	insertRow(buf, 2, "third", 5);
	delRow(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("third", (char *)buf->row[1].chars);
}

void test_del_row_end(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, "first", 5);
	insertRow(buf, 1, "second", 6);
	insertRow(buf, 2, "third", 5);
	delRow(buf, 2);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("second", (char *)buf->row[1].chars);
}

void test_row_insert_char(void) {
	struct buffer *buf = make_test_buffer("AC");
	rowInsertChar(buf, &buf->row[0], 1, 'B');
	TEST_ASSERT_EQUAL_INT(3, buf->row[0].size);
	TEST_ASSERT_EQUAL_STRING("ABC", (char *)buf->row[0].chars);
}

void test_row_del_char(void) {
	struct buffer *buf = make_test_buffer("ABC");
	rowDelChar(buf, &buf->row[0], 1);
	TEST_ASSERT_EQUAL_INT(2, buf->row[0].size);
	TEST_ASSERT_EQUAL_STRING("AC", (char *)buf->row[0].chars);
}

void test_row_append_string(void) {
	struct buffer *buf = make_test_buffer("Hello");
	rowAppendString(buf, &buf->row[0], " World", 6);
	TEST_ASSERT_EQUAL_INT(11, buf->row[0].size);
	TEST_ASSERT_EQUAL_STRING("Hello World", (char *)buf->row[0].chars);
}

void test_row_capacity_growth(void) {
	struct buffer *buf = make_test_buffer(NULL);
	for (int i = 0; i < 20; i++)
		insertRow(buf, i, "row", 3);
	TEST_ASSERT_EQUAL_INT(20, buf->numrows);
	TEST_ASSERT(buf->rowcap >= 20);
}

/* ---- Coordinate mapping ---- */

void test_chars_to_display_ascii(void) {
	struct buffer *buf = make_test_buffer("Hello");
	TEST_ASSERT_EQUAL_INT(0, charsToDisplayColumn(&buf->row[0], 0));
	TEST_ASSERT_EQUAL_INT(3, charsToDisplayColumn(&buf->row[0], 3));
	TEST_ASSERT_EQUAL_INT(5, charsToDisplayColumn(&buf->row[0], 5));
}

void test_chars_to_display_tab(void) {
	struct buffer *buf = make_test_buffer("\tA");
	TEST_ASSERT_EQUAL_INT(0, charsToDisplayColumn(&buf->row[0], 0));
	TEST_ASSERT_EQUAL_INT(8, charsToDisplayColumn(&buf->row[0], 1));
	TEST_ASSERT_EQUAL_INT(9, charsToDisplayColumn(&buf->row[0], 2));
}

void test_chars_to_display_control(void) {
	struct buffer *buf = make_test_buffer("\x01"
					      "A");
	TEST_ASSERT_EQUAL_INT(2, charsToDisplayColumn(&buf->row[0], 1));
	TEST_ASSERT_EQUAL_INT(3, charsToDisplayColumn(&buf->row[0], 2));
}

void test_chars_to_display_multibyte(void) {
	/* "A¢B" — ¢ is 2 bytes, 1 column */
	struct buffer *buf = make_test_buffer("A\xC2\xA2"
					      "B");
	TEST_ASSERT_EQUAL_INT(1, charsToDisplayColumn(&buf->row[0], 1));
	TEST_ASSERT_EQUAL_INT(2, charsToDisplayColumn(&buf->row[0], 3));
	TEST_ASSERT_EQUAL_INT(3, charsToDisplayColumn(&buf->row[0], 4));
}

void test_calculate_line_width(void) {
	struct buffer *buf = make_test_buffer("ABCDE");
	TEST_ASSERT_EQUAL_INT(5, calculateLineWidth(&buf->row[0]));
	/* Destroy before creating a new buffer to avoid orphaning this one */
	destroyBuffer(buf);
	E.headbuf = NULL;

	buf = make_test_buffer("\tX");
	TEST_ASSERT_EQUAL_INT(9, calculateLineWidth(&buf->row[1 - 1]));
}

/* ---- Screen cache ---- */

void test_build_screen_cache_no_wrap(void) {
	const char *lines[] = { "line 0", "line 1", "line 2" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->word_wrap = 0;
	buildScreenCache(buf, 80);
	TEST_ASSERT_EQUAL_INT(1, buf->screen_line_cache_valid);
	TEST_ASSERT_EQUAL_INT(0, buf->screen_line_start[0]);
	TEST_ASSERT_EQUAL_INT(1, buf->screen_line_start[1]);
	TEST_ASSERT_EQUAL_INT(2, buf->screen_line_start[2]);
}

void test_count_screen_lines_short(void) {
	struct buffer *buf = make_test_buffer("short");
	TEST_ASSERT_EQUAL_INT(1, countScreenLines(&buf->row[0], 80));
}

void test_count_screen_lines_exact(void) {
	struct buffer *buf = make_test_buffer("1234567890");
	TEST_ASSERT_EQUAL_INT(1, countScreenLines(&buf->row[0], 10));
}

void test_count_screen_lines_long(void) {
	struct buffer *buf = make_test_buffer("abcdefghijklmnopqrstuvwxy");
	int lines = countScreenLines(&buf->row[0], 10);
	TEST_ASSERT(lines >= 2);
}

void test_invalidate_screen_cache(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->word_wrap = 0;
	buildScreenCache(buf, 80);
	TEST_ASSERT_EQUAL_INT(1, buf->screen_line_cache_valid);
	invalidateScreenCache(buf);
	TEST_ASSERT_EQUAL_INT(0, buf->screen_line_cache_valid);
}

void test_word_wrap_break(void) {
	struct buffer *buf = make_test_buffer("hello world");
	int break_col, break_byte;
	int more =
		wordWrapBreak(&buf->row[0], 7, 0, 0, &break_col, &break_byte);
	TEST_ASSERT_EQUAL_INT(1, more);
	TEST_ASSERT_EQUAL_INT(6, break_col);
	TEST_ASSERT_EQUAL_INT(6, break_byte);
}

void test_cursor_screen_line(void) {
	struct buffer *buf = make_test_buffer("hello world foo");
	int out_line, out_col;
	cursorScreenLine(&buf->row[0], 0, 10, &out_line, &out_col);
	TEST_ASSERT_EQUAL_INT(0, out_line);
	TEST_ASSERT_EQUAL_INT(0, out_col);
}

/* ---- Boundary tests ---- */

void test_del_row_only_row(void) {
	struct buffer *buf = make_test_buffer("only line");
	delRow(buf, 0);
	TEST_ASSERT_EQUAL_INT(0, buf->numrows);
}

void test_row_append_string_zero_len(void) {
	struct buffer *buf = make_test_buffer("Hello");
	rowAppendString(buf, &buf->row[0], "", 0);
	TEST_ASSERT_EQUAL_INT(5, buf->row[0].size);
	TEST_ASSERT_EQUAL_STRING("Hello", (char *)buf->row[0].chars);
}

void test_row_del_char_at_start(void) {
	struct buffer *buf = make_test_buffer("ABC");
	rowDelChar(buf, &buf->row[0], 0);
	TEST_ASSERT_EQUAL_INT(2, buf->row[0].size);
	TEST_ASSERT_EQUAL_STRING("BC", (char *)buf->row[0].chars);
}

void test_row_del_char_at_end(void) {
	struct buffer *buf = make_test_buffer("ABC");
	rowDelChar(buf, &buf->row[0], 2);
	TEST_ASSERT_EQUAL_INT(2, buf->row[0].size);
	TEST_ASSERT_EQUAL_STRING("AB", (char *)buf->row[0].chars);
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_new_destroy_buffer);
	RUN_TEST(test_insert_row_beginning);
	RUN_TEST(test_insert_row_middle);
	RUN_TEST(test_insert_row_end);
	RUN_TEST(test_del_row_beginning);
	RUN_TEST(test_del_row_middle);
	RUN_TEST(test_del_row_end);
	RUN_TEST(test_row_insert_char);
	RUN_TEST(test_row_del_char);
	RUN_TEST(test_row_append_string);
	RUN_TEST(test_row_capacity_growth);

	RUN_TEST(test_chars_to_display_ascii);
	RUN_TEST(test_chars_to_display_tab);
	RUN_TEST(test_chars_to_display_control);
	RUN_TEST(test_chars_to_display_multibyte);
	RUN_TEST(test_calculate_line_width);

	RUN_TEST(test_build_screen_cache_no_wrap);
	RUN_TEST(test_count_screen_lines_short);
	RUN_TEST(test_count_screen_lines_exact);
	RUN_TEST(test_count_screen_lines_long);
	RUN_TEST(test_invalidate_screen_cache);
	RUN_TEST(test_word_wrap_break);
	RUN_TEST(test_cursor_screen_line);

	/* Boundary tests */
	RUN_TEST(test_del_row_only_row);
	RUN_TEST(test_row_append_string_zero_len);
	RUN_TEST(test_row_del_char_at_start);
	RUN_TEST(test_row_del_char_at_end);

	return TEST_END();
}
