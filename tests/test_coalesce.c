/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_coalesce.c: undo record coalescing (#104).
 *
 * The mutation layer builds every record; undo.c decides only whether
 * two given records can be folded into one.  These tests pin the
 * grouping the user sees, the cap that bounds it, and the boundaries
 * that must not be crossed. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include "mutate.h"
#include "undo.h"
#include "region.h"
#include "fileio.h"
#include <stdint.h>
#include <string.h>

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

static int nrecords(struct buffer *buf) {
	int n = 0;
	for (struct undo *u = buf->undo; u != NULL; u = u->prev)
		n++;
	return n;
}

/* A buffer in the shape editorOpen leaves: the text, plus the empty
 * final row that is the file's trailing newline.  Point starts at the
 * beginning of the text rather than on the terminator row.
 *
 * The distinction matters.  Typing on the terminator row -- or into a
 * buffer holding no text at all -- has to create the newline the
 * invariant requires, and that is a second record which closes the
 * run.  It is a property of the invariant rather than of coalescing,
 * so the coalescing tests below start clear of it and it is pinned on
 * its own in test_typing_at_end_of_buffer_costs_one_record. */
static struct buffer *loadedBuffer(const char *line) {
	struct buffer *buf = make_test_buffer(line);
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	clearUndosAndRedos(buf);
	return buf;
}

/* ---- Grouping ---- */

void test_typing_a_word_is_one_record(void) {
	struct buffer *buf = loadedBuffer("Z");

	const char *w = "hello";
	for (int i = 0; w[i]; i++)
		selfInsert(buf, w[i], 1);

	TEST_ASSERT_EQUAL_STRING("helloZ", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Z", row_str(buf, 0));
}

/* The exception, stated rather than stumbled over.  Going from no
 * lines to one line, or typing onto the terminator row, genuinely adds
 * the newline as well as the character, so the first keystroke is its
 * own undo step and the run starts from the second.  The cost is one
 * extra step per burst that begins at the end of the buffer, not one
 * per word. */
void test_typing_at_end_of_buffer_costs_one_record(void) {
	struct buffer *buf = make_test_buffer("");
	E.buf = buf;
	clearUndosAndRedos(buf);

	const char *w = "hello";
	for (int i = 0; w[i]; i++)
		selfInsert(buf, w[i], 1);

	TEST_ASSERT_EQUAL_STRING("hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 1));
	TEST_ASSERT_EQUAL_INT(3, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("h", row_str(buf, 0));
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
}

void test_backspace_run_is_one_record(void) {
	struct buffer *buf = make_test_buffer("abcdef");
	buf->cx = 6;
	E.buf = buf;
	clearUndosAndRedos(buf);

	backSpace(3);

	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("abcdef", row_str(buf, 0));
}

void test_forward_delete_run_is_one_record(void) {
	struct buffer *buf = make_test_buffer("abcdef");
	buf->cx = 0;
	E.buf = buf;
	clearUndosAndRedos(buf);

	delChar(3);

	TEST_ASSERT_EQUAL_STRING("def", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("abcdef", row_str(buf, 0));
}

/* Typing then deleting must not fold: the records describe opposite
 * operations and merging them would misstate both. */
void test_insert_then_delete_do_not_merge(void) {
	struct buffer *buf = loadedBuffer("Z");

	selfInsert(buf, 'a', 1);
	selfInsert(buf, 'b', 1);
	backSpace(1);

	TEST_ASSERT_EQUAL_INT(2, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("abZ", row_str(buf, 0));
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Z", row_str(buf, 0));
}

/* Typing away from the run's end starts a fresh record. */
void test_typing_elsewhere_starts_a_record(void) {
	struct buffer *buf = make_test_buffer("abcdef");
	buf->cx = 6;
	E.buf = buf;
	clearUndosAndRedos(buf);

	selfInsert(buf, 'X', 1);
	buf->cx = 0;
	selfInsert(buf, 'Y', 1);

	TEST_ASSERT_EQUAL_STRING("YabcdefX", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(2, nrecords(buf));
}

/* ---- The cap ---- */

/* Unbounded runs are correct but unrecoverable: one undo would discard
 * an arbitrarily long burst with no way to get part of it back. */
void test_long_run_is_capped(void) {
	struct buffer *buf = make_test_buffer("");
	E.buf = buf;
	clearUndosAndRedos(buf);

	for (int i = 0; i < 500; i++)
		selfInsert(buf, 'x', 1);

	TEST_ASSERT_EQUAL_INT(500, buf->row[0].size);
	TEST_ASSERT(nrecords(buf) >= 500 / 21);
	TEST_ASSERT(nrecords(buf) <= 500 / 20 + 2);

	while (buf->undo)
		doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 0));
}

/* The cap counts operations, not bytes, so a run of three-byte
 * characters breaks in the same place a run of ASCII does.  Before
 * #104 the two paths disagreed by a factor of three. */
void test_cap_counts_operations_not_bytes(void) {
	struct buffer *ascii = make_test_buffer("");
	E.buf = ascii;
	clearUndosAndRedos(ascii);
	for (int i = 0; i < 100; i++)
		selfInsert(ascii, 'x', 1);
	int ascii_records = nrecords(ascii);

	struct buffer *cjk = make_test_buffer("");
	cjk->next = ascii; /* keep both reachable for cleanup */
	E.buf = cjk;
	clearUndosAndRedos(cjk);
	E.nunicode = 3;
	E.unicode[0] = 0xE6;
	E.unicode[1] = 0x97;
	E.unicode[2] = 0xA5;
	for (int i = 0; i < 100; i++)
		insertUnicode(1);
	int cjk_records = nrecords(cjk);

	TEST_ASSERT_EQUAL_INT(ascii_records, cjk_records);

	while (cjk->undo)
		doUndo(cjk, 1);
	TEST_ASSERT_EQUAL_STRING("", row_str(cjk, 0));
}

/* ---- Boundaries a run must not cross ---- */

/* A bulk mutation stands alone.  Without pushUndo closing the open
 * run, typing after one could find the earlier record still aligned
 * and fold back across the bulk edit. */
void test_run_does_not_fold_across_bulk_edit(void) {
	struct buffer *buf = make_test_buffer("abc");
	buf->cx = 3;
	E.buf = buf;
	clearUndosAndRedos(buf);

	selfInsert(buf, 'X', 1);
	mutateInsert(buf, 3, 0, (const uint8_t *)"ZZ", 2, NULL, NULL);
	buf->cx = buf->row[0].size;
	selfInsert(buf, 'Y', 1);

	TEST_ASSERT_EQUAL_INT(3, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("abcZZX", row_str(buf, 0));
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("abcX", row_str(buf, 0));
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
}

/* Undo closes the run.  Typing straight afterwards can land aligned
 * with the record undo just exposed; folding into it would be correct
 * but makes "a run is one uninterrupted burst" stop holding. */
void test_undo_closes_the_run(void) {
	struct buffer *buf = loadedBuffer("Z");

	selfInsert(buf, 'a', 1);
	selfInsert(buf, 'b', 1);
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Z", row_str(buf, 0));

	selfInsert(buf, 'c', 1);
	TEST_ASSERT_EQUAL_STRING("cZ", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, nrecords(buf));
}

/* Redo re-exposes a record that may still have been open when it was
 * undone.  Typing after a redo must start a fresh one. */
void test_redo_closes_the_run(void) {
	struct buffer *buf = loadedBuffer("Z");

	selfInsert(buf, 'a', 1);
	selfInsert(buf, 'b', 1);
	doUndo(buf, 1);
	doRedo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("abZ", row_str(buf, 0));

	buf->cx = 2;
	selfInsert(buf, 'c', 1);
	TEST_ASSERT_EQUAL_STRING("abcZ", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(2, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("abZ", row_str(buf, 0));
}

/* ---- Newlines ---- */

/* RET is an ordinary insertion of the row separator, so it joins the
 * run rather than breaking it, and one undo takes the whole burst. */
void test_newline_joins_the_run(void) {
	struct buffer *buf = loadedBuffer("Z");

	selfInsert(buf, 'a', 1);
	insertNewline(1);
	selfInsert(buf, 'b', 1);

	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_INT(1, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Z", row_str(buf, 0));
}

/* ---- Anchoring at the virtual EOF ----
 *
 * The row array represents T = row[0] "\n" ... row[N-1] "\n", so it
 * cannot name the position after the final newline.  An insertion at
 * the virtual EOF line therefore changes one more byte than it looks
 * like, and the record has to say so or undo leaves the row behind
 * (#102).  Unifying the paths does not remove the need — see the EOF
 * issue for the change that would. */
/* Both tests below assert the serialised buffer rather than numrows.
 * The behaviour being characterised is "an edit at the end of the
 * buffer undoes back to exactly the original text, leaving no row
 * behind" -- a statement about text, not about row counts.  The
 * expected strings are derived from the representation table in
 * buffer.h, not from observing what the code does: a two-line
 * newline-terminated file is "one\ntwo\n" before and after the undo,
 * whatever the row array happens to look like underneath.
 *
 * Asserting numrows here would tie the test to the representation
 * and, worse, would make it satisfiable by whatever count was last
 * observed.  These assertions catch a failed restore either way. */
static void assertContent(struct buffer *buf, const char *want) {
	size_t len = 0;
	char *got = rowsToString(buf, &len);
	TEST_ASSERT_EQUAL_INT((int)strlen(want), (int)len);
	if (len == strlen(want))
		TEST_ASSERT_EQUAL_INT(0, memcmp(want, got, len));
	free(got);
}

void test_newline_at_virtual_eof_undoes_cleanly(void) {
	const char *lines[] = { "one", "two" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 0;
	buf->cy = buf->numrows - 1; /* end of buffer */
	E.buf = buf;
	clearUndosAndRedos(buf);
	assertContent(buf, "one\ntwo\n");

	insertNewline(1);
	assertContent(buf, "one\ntwo\n\n");

	doUndo(buf, 1);
	assertContent(buf, "one\ntwo\n");
}

void test_typing_at_virtual_eof_undoes_cleanly(void) {
	const char *lines[] = { "one", "two" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 0;
	buf->cy = buf->numrows - 1;
	E.buf = buf;
	clearUndosAndRedos(buf);

	selfInsert(buf, 'X', 1);
	selfInsert(buf, 'Y', 1);
	/* The terminator the invariant requires is part of the text
	 * now, so the buffer reads as a three-line file. */
	assertContent(buf, "one\ntwo\nXY\n");

	while (buf->undo)
		doUndo(buf, 1);
	assertContent(buf, "one\ntwo\n");
}

/* ---- Final-newline invariant, maintenance half ----
 *
 * Load establishes it; the mutation layer keeps it.  Two behaviours:
 * a deletion of nothing but the final newline is refused outright,
 * and a deletion that takes it along with real text is allowed and
 * the terminator re-appended in the same undo step. */

/* Refused, and refused before anything is recorded: no undo record, no
 * dirty flag, no cleared redo stack.  Doing it as a delete followed by
 * a repair would leave the buffer byte-identical but two records
 * heavier and spuriously modified. */
void test_deleting_only_the_final_newline_is_refused(void) {
	struct buffer *buf = make_test_buffer("abc");
	buf->cx = 3;
	buf->cy = 0;
	E.buf = buf;
	clearUndosAndRedos(buf);
	buf->dirty = 0;

	delChar(1);

	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 1));
	TEST_ASSERT_EQUAL_INT(0, nrecords(buf));
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}

/* The same refusal reached from the other side.  Backspace at the
 * start of the terminator row asks to delete exactly the same byte. */
void test_backspace_onto_the_terminator_row_is_refused(void) {
	struct buffer *buf = make_test_buffer("abc");
	buf->cx = 0;
	buf->cy = 1;
	E.buf = buf;
	clearUndosAndRedos(buf);
	buf->dirty = 0;

	backSpace(1);

	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(0, nrecords(buf));
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}

/* Taking the final newline along with real text is a real edit, so it
 * proceeds; the terminator is re-appended.  The repair is paired, so
 * one undo takes the whole thing rather than exposing an intermediate
 * state that never existed. */
void test_deleting_past_the_end_repairs_in_one_step(void) {
	const char *lines[] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 2;
	buf->cy = 1;
	E.buf = buf;
	clearUndosAndRedos(buf);
	assertContent(buf, "alpha\nbeta\n");

	/* From inside the last line of text through the terminator. */
	deleteRange(2, 1, 0, 2, 0);

	assertContent(buf, "alpha\nbe\n");
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, buf->numrows - 1));

	doUndo(buf, 1);
	assertContent(buf, "alpha\nbeta\n");
}

/* The refusal is narrower than "the target is the final newline": it
 * applies only where the repair would undo the delete.  An empty
 * penultimate row leaves the invariant holding, so removing a trailing
 * blank line is a real edit. */
void test_backspace_deletes_a_trailing_blank_line(void) {
	const char *lines[] = { "a", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	E.buf = buf;
	clearUndosAndRedos(buf);
	buf->dirty = 0;
	assertContent(buf, "a\n\n");

	buf->cx = 0;
	buf->cy = 2;
	backSpace(1);

	assertContent(buf, "a\n");
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, buf->numrows - 1));
	TEST_ASSERT_EQUAL_INT(1, nrecords(buf));
	TEST_ASSERT_EQUAL_INT(1, buf->dirty);

	doUndo(buf, 1);
	assertContent(buf, "a\n\n");
}

/* The same edit reached with C-d from the end of the row above. */
void test_delete_forward_removes_a_trailing_blank_line(void) {
	const char *lines[] = { "a", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	E.buf = buf;
	clearUndosAndRedos(buf);
	buf->dirty = 0;

	buf->cx = 0;
	buf->cy = 1;
	delChar(1);

	assertContent(buf, "a\n");
	doUndo(buf, 1);
	assertContent(buf, "a\n\n");
}

/* A buffer holding only a newline empties, satisfying the invariant by
 * the bufferIsEmpty arm. */
void test_backspace_on_lone_blank_line_empties_the_buffer(void) {
	const char *lines[] = { "" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	E.buf = buf;
	clearUndosAndRedos(buf);
	buf->dirty = 0;
	assertContent(buf, "\n");

	buf->cx = 0;
	buf->cy = 1;
	backSpace(1);

	assertContent(buf, "");
	TEST_ASSERT_EQUAL_INT(1, bufferIsEmpty(buf));

	doUndo(buf, 1);
	assertContent(buf, "\n");
}

/* ---- Round trip ---- */

/* The property the fuzzer asserts, pinned for a run that crosses
 * several cap boundaries and both operation kinds. */
void test_mixed_burst_round_trips(void) {
	const char *lines[] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 5;
	buf->cy = 0;
	E.buf = buf;
	clearUndosAndRedos(buf);

	for (int i = 0; i < 57; i++)
		selfInsert(buf, 'q', 1);
	insertNewline(1);
	for (int i = 0; i < 30; i++)
		selfInsert(buf, 'w', 1);
	backSpace(25);

	while (buf->undo)
		doUndo(buf, 1);

	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("alpha", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("beta", row_str(buf, 1));
}

/* An explicit prefix argument is one command.  C-u 100 x inserts a
 * hundred characters, but undoing it must take one keystroke, not
 * five: the cap chops up a run of separate commands, not a single
 * command that happens to repeat. */
void test_prefix_argument_repeat_is_one_record(void) {
	struct buffer *buf = loadedBuffer("Z");

	selfInsert(buf, 'x', 100);

	TEST_ASSERT_EQUAL_INT(101, buf->row[0].size);
	TEST_ASSERT_EQUAL_INT(1, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Z", row_str(buf, 0));
}

void test_prefix_argument_newlines_are_one_record(void) {
	struct buffer *buf = make_test_buffer("ab");
	buf->cx = 1;
	E.buf = buf;
	clearUndosAndRedos(buf);

	insertNewline(30);

	TEST_ASSERT_EQUAL_INT(32, buf->numrows);
	TEST_ASSERT_EQUAL_INT(1, nrecords(buf));

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("ab", row_str(buf, 0));
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_typing_a_word_is_one_record);
	RUN_TEST(test_typing_at_end_of_buffer_costs_one_record);
	RUN_TEST(test_backspace_run_is_one_record);
	RUN_TEST(test_forward_delete_run_is_one_record);
	RUN_TEST(test_insert_then_delete_do_not_merge);
	RUN_TEST(test_typing_elsewhere_starts_a_record);

	RUN_TEST(test_long_run_is_capped);
	RUN_TEST(test_cap_counts_operations_not_bytes);
	RUN_TEST(test_prefix_argument_repeat_is_one_record);
	RUN_TEST(test_prefix_argument_newlines_are_one_record);

	RUN_TEST(test_run_does_not_fold_across_bulk_edit);
	RUN_TEST(test_undo_closes_the_run);
	RUN_TEST(test_redo_closes_the_run);

	RUN_TEST(test_newline_joins_the_run);

	RUN_TEST(test_deleting_only_the_final_newline_is_refused);
	RUN_TEST(test_backspace_onto_the_terminator_row_is_refused);
	RUN_TEST(test_deleting_past_the_end_repairs_in_one_step);
	RUN_TEST(test_backspace_deletes_a_trailing_blank_line);
	RUN_TEST(test_delete_forward_removes_a_trailing_blank_line);
	RUN_TEST(test_backspace_on_lone_blank_line_empties_the_buffer);

	RUN_TEST(test_newline_at_virtual_eof_undoes_cleanly);
	RUN_TEST(test_typing_at_virtual_eof_undoes_cleanly);

	RUN_TEST(test_mixed_burst_round_trips);

	return TEST_END();
}
