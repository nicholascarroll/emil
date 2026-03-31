/* test_undo.c — Undo/redo stack, coalescing, bulk replay.
 * Highest-value test target: undo bugs silently corrupt files. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include <stdint.h>

/* ---- Basic undo/redo ---- */

void test_undo_insert_chars(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 5;

	const char *text = " World";
	for (int i = 0; text[i]; i++) {
		undoAppendChar(buf, text[i]);
		rowInsertChar(buf, &buf->row[0], buf->cx, text[i]);
		buf->cx++;
	}
	TEST_ASSERT_EQUAL_STRING("Hello World", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
}

void test_undo_then_redo(void) {
	struct buffer *buf = make_test_buffer("ABC");
	buf->cx = 3;

	undoAppendChar(buf, 'D');
	rowInsertChar(buf, &buf->row[0], buf->cx, 'D');
	buf->cx++;
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("ABC", row_str(buf, 0));

	doRedo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
}

void test_multiple_sequential_undos(void) {
	struct buffer *buf = make_test_buffer("A");
	buf->cx = 1;

	undoAppendChar(buf, 'B');
	rowInsertChar(buf, &buf->row[0], buf->cx, 'B');
	buf->cx++;
	buf->undo->append = 0; /* Force new record */

	undoAppendChar(buf, 'C');
	rowInsertChar(buf, &buf->row[0], buf->cx, 'C');
	buf->cx++;
	TEST_ASSERT_EQUAL_STRING("ABC", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("AB", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("A", row_str(buf, 0));
}

void test_undo_delete_chars(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 4;

	undoDelChar(buf, &buf->row[0]);
	rowDelChar(buf, &buf->row[0], buf->cx);
	TEST_ASSERT_EQUAL_STRING("Hell", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
}

/* ---- Coalescing ---- */

void test_coalesce_consecutive_inserts(void) {
	struct buffer *buf = make_test_buffer("");
	insertRow(buf, 0, "", 0);
	buf->cx = 0;
	clearUndosAndRedos(buf);

	undoAppendChar(buf, 'A');
	rowInsertChar(buf, &buf->row[0], buf->cx, 'A');
	buf->cx++;
	undoAppendChar(buf, 'B');
	rowInsertChar(buf, &buf->row[0], buf->cx, 'B');
	buf->cx++;
	undoAppendChar(buf, 'C');
	rowInsertChar(buf, &buf->row[0], buf->cx, 'C');
	buf->cx++;
	TEST_ASSERT_EQUAL_STRING("ABC", row_str(buf, 0));

	/* Should undo as a single record */
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 0));
}

void test_backspace_coalescing(void) {
	struct buffer *buf = make_test_buffer("ABCD");
	buf->cx = 4;
	clearUndosAndRedos(buf);

	buf->cx--;
	undoBackSpace(buf, buf->row[0].chars[buf->cx]);
	rowDelChar(buf, &buf->row[0], buf->cx);

	buf->cx--;
	undoBackSpace(buf, buf->row[0].chars[buf->cx]);
	rowDelChar(buf, &buf->row[0], buf->cx);

	TEST_ASSERT_EQUAL_STRING("AB", row_str(buf, 0));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
}

void test_forward_delete_coalescing(void) {
	struct buffer *buf = make_test_buffer("ABCD");
	buf->cx = 0;
	clearUndosAndRedos(buf);

	undoDelChar(buf, &buf->row[0]);
	rowDelChar(buf, &buf->row[0], buf->cx);

	undoDelChar(buf, &buf->row[0]);
	rowDelChar(buf, &buf->row[0], buf->cx);

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

	undoAppendChar(buf, 'B');
	rowInsertChar(buf, &buf->row[0], buf->cx, 'B');
	buf->cx++;

	doUndo(buf, 1);
	TEST_ASSERT_NOT_NULL(buf->redo);

	buf->cx = 1;
	undoAppendChar(buf, 'C');
	rowInsertChar(buf, &buf->row[0], buf->cx, 'C');
	buf->cx++;
	TEST_ASSERT_NULL(buf->redo);
	TEST_ASSERT_EQUAL_STRING("AC", row_str(buf, 0));
}

/* ---- Multi-line ---- */

void test_undo_newline_insert(void) {
	struct buffer *buf = make_test_buffer("HelloWorld");
	buf->cx = 5;
	E.buf = buf;
	insertNewline(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("World", row_str(buf, 1));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("HelloWorld", row_str(buf, 0));
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
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

	return TEST_END();
}
