/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_undo.c: Undo/redo stack, coalescing, bulk replay.
 * Highest-value test target: undo bugs silently corrupt files. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include "mutate.h"
#include "undo.h"
#include <stdint.h>

/* ---- Basic undo/redo ---- */

void test_undo_insert_chars(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 5;

	const char *text = " World";
	for (int i = 0; text[i]; i++)
		selfInsert(buf, text[i], 1);
	TEST_ASSERT_EQUAL_STRING("Hello World", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
}

void test_undo_then_redo(void) {
	struct buffer *buf = make_test_buffer("ABC");
	buf->cx = 3;

	selfInsert(buf, 'D', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("ABC", row_str(buf, 0));

	doRedo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
}

void test_multiple_sequential_undos(void) {
	struct buffer *buf = make_test_buffer("A");
	buf->cx = 1;

	selfInsert(buf, 'B', 1);
	buf->undo->append = 0; /* Force new record */

	selfInsert(buf, 'C', 1);
	TEST_ASSERT_EQUAL_STRING("ABC", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("AB", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("A", row_str(buf, 0));
}

void test_undo_delete_chars(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 4;

	E.buf = buf;
	delChar(1);
	TEST_ASSERT_EQUAL_STRING("Hell", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
}

/* ---- Coalescing ---- */

void test_coalesce_consecutive_inserts(void) {
	struct buffer *buf = make_test_buffer("");
	insertRow(buf, 0, (const uint8_t *)"", 0);
	buf->cx = 0;
	clearUndosAndRedos(buf);

	selfInsert(buf, 'A', 1);
	selfInsert(buf, 'B', 1);
	selfInsert(buf, 'C', 1);
	TEST_ASSERT_EQUAL_STRING("ABC", row_str(buf, 0));

	/* Should undo as a single record */
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 0));
}

void test_backspace_coalescing(void) {
	struct buffer *buf = make_test_buffer("ABCD");
	buf->cx = 4;
	clearUndosAndRedos(buf);

	E.buf = buf;
	backSpace(1);
	backSpace(1);

	TEST_ASSERT_EQUAL_STRING("AB", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
}

void test_forward_delete_coalescing(void) {
	struct buffer *buf = make_test_buffer("ABCD");
	buf->cx = 0;
	clearUndosAndRedos(buf);

	E.buf = buf;
	delChar(1);
	delChar(1);

	TEST_ASSERT_EQUAL_STRING("CD", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
}

/* ---- Edge cases ---- */

void test_undo_empty_stack(void) {
	struct buffer *buf = make_test_buffer("Hello");
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
}

void test_redo_cleared_after_new_edit(void) {
	struct buffer *buf = make_test_buffer("A");
	buf->cx = 1;

	selfInsert(buf, 'B', 1);

	doUndo(buf, 1);
	TEST_ASSERT_NOT_NULL(buf->redo);

	buf->cx = 1;
	selfInsert(buf, 'C', 1);
	TEST_ASSERT_NULL(buf->redo);
	TEST_ASSERT_EQUAL_STRING("AC", row_str(buf, 0));
}

/* ---- Multi-line ---- */

void test_undo_newline_insert(void) {
	struct buffer *buf = make_test_buffer("HelloWorld");
	buf->cx = 5;
	E.buf = buf;
	insertNewline(1);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("World", row_str(buf, 1));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("HelloWorld", row_str(buf, 0));
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- Mutation-layer read-only choke point ---- */

/* mutateReplace must refuse read-only buffers before clearRedos:
 * buffer unchanged, no undo pushed, redo stack untouched. */
void test_mutate_replace_readonly(void) {
	struct buffer *buf = make_test_buffer("Hello");

	/* Seed a redo record: one real insert, then undo it. */
	buf->cx = 0;
	selfInsert(buf, 'X', 1);
	doUndo(buf, 1);
	TEST_ASSERT_NOT_NULL(buf->redo);

	buf->read_only = 1;
	mutateReplace(buf, 0, 0, 5, 0, (const uint8_t *)"Hello", 5,
		      (const uint8_t *)"bye", 3, 0, NULL, NULL);

	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_NULL(buf->undo);
	TEST_ASSERT_NOT_NULL(buf->redo);
	buf->read_only = 0;
}

/* ---- Quoted newline (C-q C-j) ----
 *
 * '\n' is emil's row separator, never a byte stored inside a row.
 * CMD_QUOTED_INSERT therefore splits the row rather than calling
 * insertChar, which would have written a literal 0x0A while the undo
 * record described a row split -- coordinates and buffer then
 * disagreed, and undo destroyed the rest of the line plus the row
 * below.  These two pin the invariant from both directions. */

void test_quoted_newline_undo_preserves_following_row(void) {
	const char *lines[] = { "abcdef", "ghi" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 3;
	buf->cy = 0;
	clearUndosAndRedos(buf);

	insertNewline(1);
	TEST_ASSERT_EQUAL_INT(4, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("def", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("ghi", row_str(buf, 2));

	/* Previously produced a single row, "abcghi". */
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("abcdef", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("ghi", row_str(buf, 1));
}

/* The row-join branch of backSpace used to derive the record's start
 * itself, reading buf->row[cy - 1] with no guarantee that cy >= 1.
 * The mutation layer is now handed both coordinates by the caller, so
 * the read is gone rather than guarded.  Backspace at the origin must
 * be a no-op; run under `make asan` to check it stays one. */
void test_backspace_at_origin_is_a_noop(void) {
	struct buffer *buf = make_test_buffer("ab");
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	clearUndosAndRedos(buf);

	backSpace(1);

	TEST_ASSERT_EQUAL_STRING("ab", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_NULL(buf->undo);
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_undo_insert_chars);
	RUN_TEST(test_undo_then_redo);
	RUN_TEST(test_multiple_sequential_undos);
	RUN_TEST(test_undo_delete_chars);

	RUN_TEST(test_coalesce_consecutive_inserts);
	RUN_TEST(test_backspace_coalescing);
	RUN_TEST(test_forward_delete_coalescing);

	RUN_TEST(test_undo_empty_stack);
	RUN_TEST(test_redo_cleared_after_new_edit);

	RUN_TEST(test_undo_newline_insert);
	RUN_TEST(test_quoted_newline_undo_preserves_following_row);
	RUN_TEST(test_backspace_at_origin_is_a_noop);

	RUN_TEST(test_mutate_replace_readonly);

	return TEST_END();
}
