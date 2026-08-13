/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_buffer.c: Row operations, screen cache, coordinate mapping. */

#include "test.h"
#include "test_harness.h"
#include "prompt.h"
#include "completion.h"
#include "buffer.h"
#include "util.h"
#include <stdint.h>

/* ---- Kill-buffer confirmation ----
 *
 * Matches kill-buffer in Emacs: confirm only for a modified buffer
 * that is visiting a file.  A modified buffer with no filename is
 * scratch space and is killed without a prompt. */

void test_kill_confirm_dirty_named_buffer(void) {
	struct buffer *buf = make_test_buffer("unsaved work");
	buf->dirty = 1;
	buf->filename = xstrdup("/tmp/notes.txt");
	buf->special_buffer = 0;
	TEST_ASSERT_TRUE(killBufferNeedsConfirm(buf));
}

void test_kill_no_confirm_dirty_unnamed_buffer(void) {
	struct buffer *buf = make_test_buffer("scratch jottings");
	buf->dirty = 1;
	buf->filename = NULL;
	buf->special_buffer = 0;
	TEST_ASSERT_FALSE(killBufferNeedsConfirm(buf));
}

void test_kill_no_confirm_clean_named_buffer(void) {
	struct buffer *buf = make_test_buffer("saved");
	buf->dirty = 0;
	buf->filename = xstrdup("/tmp/notes.txt");
	buf->special_buffer = 0;
	TEST_ASSERT_FALSE(killBufferNeedsConfirm(buf));
}

/* A special buffer carries a filename ("*scratch*", "*Completions*"),
 * so special_buffer is what suppresses the prompt here, not the
 * filename test. */
void test_kill_no_confirm_special_buffer_with_name(void) {
	struct buffer *buf = make_test_buffer("completions");
	buf->dirty = 1;
	buf->filename = xstrdup("*scratch*");
	buf->special_buffer = 1;
	TEST_ASSERT_FALSE(killBufferNeedsConfirm(buf));
}

/* ---- Row operations ---- */

void test_new_destroy_buffer(void) {
	struct buffer *buf = newBuffer();
	TEST_ASSERT_NOT_NULL(buf);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_NULL(buf->filename);
	destroyBuffer(buf);
}

void test_insert_row_beginning(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, (const uint8_t *)"second", 6);
	insertRow(buf, 0, (const uint8_t *)"first", 5);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("first", (char *)buf->row[0].chars);
	TEST_ASSERT_EQUAL_STRING("second", (char *)buf->row[1].chars);
}

void test_insert_row_end(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, (const uint8_t *)"first", 5);
	insertRow(buf, 1, (const uint8_t *)"second", 6);
	insertRow(buf, 2, (const uint8_t *)"third", 5);
	TEST_ASSERT_EQUAL_INT(4, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("third", (char *)buf->row[2].chars);
}

void test_del_row_beginning(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, (const uint8_t *)"first", 5);
	insertRow(buf, 1, (const uint8_t *)"second", 6);
	insertRow(buf, 2, (const uint8_t *)"third", 5);
	delRow(buf, 0);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("second", (char *)buf->row[0].chars);
}

void test_del_row_end(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, (const uint8_t *)"first", 5);
	insertRow(buf, 1, (const uint8_t *)"second", 6);
	insertRow(buf, 2, (const uint8_t *)"third", 5);
	delRow(buf, 2);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("second", (char *)buf->row[1].chars);
}

void test_row_insert_char(void) {
	struct buffer *buf = make_test_buffer("AC");
	rowInsertChar(buf, &buf->row[0], 1, 'B');
	TEST_ASSERT_EQUAL_INT(3, buf->row[0].size);
	TEST_ASSERT_EQUAL_STRING("ABC", (char *)buf->row[0].chars);
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

/* ---- Screen line counting ---- */

void test_count_screen_lines_exact(void) {
	struct buffer *buf = make_test_buffer("1234567890");
	TEST_ASSERT_EQUAL_INT(1, countScreenLines(&buf->row[0], 10));
}

void test_count_screen_lines_long(void) {
	struct buffer *buf = make_test_buffer("abcdefghijklmnopqrstuvwxy");
	int lines = countScreenLines(&buf->row[0], 10);
	TEST_ASSERT(lines >= 2);
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

/* ---- Boundary tests ---- */

void test_del_row_only_row(void) {
	struct buffer *buf = make_test_buffer("only line");
	delRow(buf, 0);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- Minibuffer serialization ----
 *
 * The minibuffer holds rows like any other buffer; no row ever contains
 * a literal 0x0A.  Text crosses the boundary in two forms: joined with
 * "\n" for the caller, joined with "^J" for the screen. */

void test_minibuf_roundtrip_multiline(void) {
	struct buffer *mb = newBuffer();
	replaceMinibufferText(mb, "foo\nbar");

	TEST_ASSERT_EQUAL_INT(2, mb->numrows);
	TEST_ASSERT_EQUAL_STRING("foo", row_str(mb, 0));
	TEST_ASSERT_EQUAL_STRING("bar", row_str(mb, 1));

	char *out = minibufJoin(mb, "\n");
	TEST_ASSERT_EQUAL_STRING("foo\nbar", out);
	free(out);
	destroyBuffer(mb);
}

void test_minibuf_display_uses_caret(void) {
	struct buffer *mb = newBuffer();
	replaceMinibufferText(mb, "a\nb\nc");

	TEST_ASSERT_EQUAL_INT(3, mb->numrows);
	char *shown = minibufJoin(mb, "^J");
	TEST_ASSERT_EQUAL_STRING("a^Jb^Jc", shown);
	free(shown);
	destroyBuffer(mb);
}

void test_minibuf_single_line_unchanged(void) {
	struct buffer *mb = newBuffer();
	replaceMinibufferText(mb, "plain");

	TEST_ASSERT_EQUAL_INT(1, mb->numrows);
	char *out = minibufJoin(mb, "\n");
	TEST_ASSERT_EQUAL_STRING("plain", out);
	free(out);
	destroyBuffer(mb);
}

/* Typing only C-q C-j: two rows, both empty, value "\n".  This is
 * the line-join entry; editorPrompt must not submit it as "". */
void test_minibuf_bare_newline_is_not_empty(void) {
	struct buffer *mb = newBuffer();
	replaceMinibufferText(mb, "\n");

	TEST_ASSERT_EQUAL_INT(2, mb->numrows);
	char *out = minibufJoin(mb, "\n");
	TEST_ASSERT_EQUAL_STRING("\n", out);
	free(out);
	destroyBuffer(mb);
}

/* User text embedded into a prompt or status string must never carry
 * a raw 0x0A to the terminal; it is rewritten in caret notation. */
void test_caret_escape_newlines(void) {
	char *out = caretEscapeNewlines((const uint8_t *)"a\nb");
	TEST_ASSERT_EQUAL_STRING("a^Jb", out);
	free(out);

	out = caretEscapeNewlines((const uint8_t *)"\n");
	TEST_ASSERT_EQUAL_STRING("^J", out);
	free(out);

	out = caretEscapeNewlines((const uint8_t *)"plain");
	TEST_ASSERT_EQUAL_STRING("plain", out);
	free(out);

	out = caretEscapeNewlines((const uint8_t *)"");
	TEST_ASSERT_EQUAL_STRING("", out);
	free(out);
}

void test_minibuf_trailing_newline(void) {
	struct buffer *mb = newBuffer();
	replaceMinibufferText(mb, "x\n");

	/* A pattern ending in a row break: two rows, the second empty. */
	TEST_ASSERT_EQUAL_INT(2, mb->numrows);
	char *out = minibufJoin(mb, "\n");
	TEST_ASSERT_EQUAL_STRING("x\n", out);
	free(out);
	destroyBuffer(mb);
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_new_destroy_buffer);
	RUN_TEST(test_insert_row_beginning);
	RUN_TEST(test_insert_row_end);
	RUN_TEST(test_del_row_beginning);
	RUN_TEST(test_del_row_end);
	RUN_TEST(test_row_insert_char);

	RUN_TEST(test_chars_to_display_ascii);
	RUN_TEST(test_chars_to_display_tab);
	RUN_TEST(test_chars_to_display_control);
	RUN_TEST(test_chars_to_display_multibyte);
	RUN_TEST(test_calculate_line_width);

	RUN_TEST(test_count_screen_lines_exact);
	RUN_TEST(test_count_screen_lines_long);
	RUN_TEST(test_word_wrap_break);

	/* Minibuffer serialization */
	RUN_TEST(test_minibuf_roundtrip_multiline);
	RUN_TEST(test_minibuf_display_uses_caret);
	RUN_TEST(test_minibuf_single_line_unchanged);
	RUN_TEST(test_minibuf_trailing_newline);
	RUN_TEST(test_minibuf_bare_newline_is_not_empty);
	RUN_TEST(test_caret_escape_newlines);

	/* Boundary tests */
	RUN_TEST(test_del_row_only_row);

	/* Kill-buffer confirmation */
	RUN_TEST(test_kill_confirm_dirty_named_buffer);
	RUN_TEST(test_kill_no_confirm_dirty_unnamed_buffer);
	RUN_TEST(test_kill_no_confirm_clean_named_buffer);
	RUN_TEST(test_kill_no_confirm_special_buffer_with_name);

	return TEST_END();
}
