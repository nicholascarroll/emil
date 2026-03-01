/* test_edit.c — Parameterized edit primitives. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include <stdint.h>

/* ---- Character insertion ---- */

void test_insert_char_beginning(void) {
	struct editorBuffer *buf = make_test_buffer("BCD");
	buf->cx = 0;
	editorInsertChar(buf, 'A', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	destroyBuffer(buf);
}

void test_insert_char_middle(void) {
	struct editorBuffer *buf = make_test_buffer("ACD");
	buf->cx = 1;
	editorInsertChar(buf, 'B', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_insert_char_end(void) {
	struct editorBuffer *buf = make_test_buffer("ABC");
	buf->cx = 3;
	editorInsertChar(buf, 'D', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_insert_char_with_count(void) {
	struct editorBuffer *buf = make_test_buffer("AE");
	buf->cx = 1;
	editorInsertChar(buf, 'B', 3);
	TEST_ASSERT_EQUAL_STRING("ABBBE", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(4, buf->cx);
	destroyBuffer(buf);
}

void test_insert_char_readonly(void) {
	struct editorBuffer *buf = make_test_buffer("Hello");
	buf->read_only = 1;
	buf->cx = 0;
	editorInsertChar(buf, 'X', 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	destroyBuffer(buf);
}

/* ---- Newlines ---- */

void test_insert_newline_splits(void) {
	struct editorBuffer *buf = make_test_buffer("HelloWorld");
	buf->cx = 5;
	editorInsertNewline(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("World", row_str(buf, 1));
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
	TEST_ASSERT_EQUAL_INT(1, buf->cy);
	destroyBuffer(buf);
}

void test_insert_newline_at_beginning(void) {
	struct editorBuffer *buf = make_test_buffer("Hello");
	buf->cx = 0;
	editorInsertNewline(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 1));
	destroyBuffer(buf);
}

void test_insert_newline_at_end(void) {
	struct editorBuffer *buf = make_test_buffer("Hello");
	buf->cx = 5;
	editorInsertNewline(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 1));
	destroyBuffer(buf);
}

void test_insert_newline_and_indent(void) {
	struct editorBuffer *buf = make_test_buffer("    Hello");
	buf->cx = 9;
	editorInsertNewlineAndIndent(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("    Hello", row_str(buf, 0));
	TEST_ASSERT(buf->row[1].size >= 4);
	TEST_ASSERT(buf->row[1].chars[0] == ' ');
	TEST_ASSERT(buf->row[1].chars[3] == ' ');
	destroyBuffer(buf);
}

void test_open_line(void) {
	struct editorBuffer *buf = make_test_buffer("Hello");
	buf->cx = 5;
	editorOpenLine(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT_EQUAL_INT(5, buf->cx);
	destroyBuffer(buf);
}

/* ---- Deletion ---- */

void test_del_char_middle(void) {
	struct editorBuffer *buf = make_test_buffer("ABCD");
	buf->cx = 1;
	editorDelChar(buf, 1);
	TEST_ASSERT_EQUAL_STRING("ACD", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_del_char_joins_lines(void) {
	const char *lines[] = { "Hello", "World" };
	struct editorBuffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 5;
	buf->cy = 0;
	editorDelChar(buf, 1);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("HelloWorld", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_backspace_middle(void) {
	struct editorBuffer *buf = make_test_buffer("ABCD");
	buf->cx = 2;
	editorBackSpace(buf, 1);
	TEST_ASSERT_EQUAL_STRING("ACD", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	destroyBuffer(buf);
}

void test_backspace_joins_lines(void) {
	const char *lines[] = { "Hello", "World" };
	struct editorBuffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 0;
	buf->cy = 1;
	editorBackSpace(buf, 1);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("HelloWorld", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(5, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	destroyBuffer(buf);
}

/* ---- Indentation ---- */

void test_indent_tab(void) {
	struct editorBuffer *buf = make_test_buffer("Hello");
	buf->cx = 0;
	buf->indent = 0;
	editorIndent(buf, 1);
	TEST_ASSERT_EQUAL_STRING("\tHello", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_indent_spaces(void) {
	struct editorBuffer *buf = make_test_buffer("Hello");
	buf->cx = 0;
	buf->indent = 4;
	editorIndent(buf, 1);
	TEST_ASSERT_EQUAL_STRING("    Hello", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_unindent(void) {
	struct editorBuffer *buf = make_test_buffer("\tHello");
	buf->cx = 1;
	buf->indent = 0;
	editorUnindent(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	destroyBuffer(buf);
}

void setUp(void) { initTestEditor(); }
void tearDown(void) {}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_insert_char_beginning);
	RUN_TEST(test_insert_char_middle);
	RUN_TEST(test_insert_char_end);
	RUN_TEST(test_insert_char_with_count);
	RUN_TEST(test_insert_char_readonly);

	RUN_TEST(test_insert_newline_splits);
	RUN_TEST(test_insert_newline_at_beginning);
	RUN_TEST(test_insert_newline_at_end);
	RUN_TEST(test_insert_newline_and_indent);
	RUN_TEST(test_open_line);

	RUN_TEST(test_del_char_middle);
	RUN_TEST(test_del_char_joins_lines);
	RUN_TEST(test_backspace_middle);
	RUN_TEST(test_backspace_joins_lines);

	RUN_TEST(test_indent_tab);
	RUN_TEST(test_indent_spaces);
	RUN_TEST(test_unindent);

	return TEST_END();
}
