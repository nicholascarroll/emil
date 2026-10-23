/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_coalesce.c: how edits group into undo steps (#104).
 *
 * These tests pin the grouping the user sees, the cap that bounds it,
 * and the boundaries that must not be crossed.
 *
 * A group is checked by what one undo takes back: the tests issue
 * editing commands through processKeypress(), press undo, and read
 * the text.  They do not count records or look at how a record is
 * built, so undo's storage and the code that decides a merge can be
 * reshaped without touching this file. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include "keymap.h"
#include "undo.h"
#include "fileio.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- Keys ---- */

static void type_char(int ch) {
	E.self_insert_key = ch;
	processKeypress(CMD_SELF_INSERT);
}

static void type_str(const char *s) {
	for (; *s; s++)
		type_char((unsigned char)*s);
}

/* The same command n times over, as n separate key presses. */
static void press_times(int cmd, int n) {
	for (int i = 0; i < n; i++)
		processKeypress(cmd);
}

/* C-u n, then the command: one command with a prefix argument. */
static void press_with_arg(int n, int cmd) {
	E.uarg = n;
	processKeypress(cmd);
}

static void undo(void) {
	processKeypress(CMD_UNDO);
}

static void redo(void) {
	processKeypress(CMD_REDO);
}

/* ---- Text ---- */

/* The buffer as the bytes a save would write.  Caller frees. */
static char *contentOf(struct buffer *buf) {
	size_t len = 0;
	return rowsToString(buf, &len);
}

/* The assertions below read the serialised buffer rather than row
 * counts.  The behaviour being characterised is a statement about
 * text -- "this undo takes back exactly that" -- and the expected
 * strings are derived from the representation table in buffer.h, not
 * from observing what the code does: a two-line newline-terminated
 * file is "one\ntwo\n", whatever the row array happens to look like
 * underneath. */
static void assertContent(struct buffer *buf, const char *want) {
	char *got = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(want, got);
	free(got);
}

/* Press undo until it stops changing the text, and say how many
 * presses changed it: the number of undo steps the user has. */
static int undoStepsToStart(struct buffer *buf) {
	int steps = 0;
	for (int i = 0; i < 2000; i++) {
		char *before = contentOf(buf);
		undo();
		char *after = contentOf(buf);
		int changed = strcmp(before, after) != 0;
		free(before);
		free(after);
		if (!changed)
			break;
		steps++;
	}
	return steps;
}

/* A buffer in the shape editorOpen leaves: the text, plus the empty
 * final row that is the file's trailing newline.  Point starts at the
 * beginning of the text rather than on the terminator row.
 *
 * The distinction matters.  Typing on the terminator row -- or into a
 * buffer holding no text at all -- has to create the newline the
 * invariant requires, and that closes the run.  It is a property of
 * the invariant rather than of coalescing, so the coalescing tests
 * below start clear of it and it is pinned on its own in
 * test_typing_at_end_of_buffer_costs_one_record. */
static struct buffer *loadedBuffer(const char *line) {
	struct buffer *buf = make_test_buffer(line);
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	clearUndosAndRedos(buf);
	return buf;
}

/* Put `text` (one line) on the kill ring the way a user would: select
 * it in a scratch buffer and copy it (M-w).  The scratch buffer is
 * gone on return, so call this before making the buffer under test. */
static void copyToKillRing(const char *text) {
	struct buffer *scratch = make_test_buffer(text);
	scratch->markx = 0;
	scratch->marky = 0;
	scratch->mark_active = 1;
	scratch->cx = scratch->row[0].size;
	scratch->cy = 0;
	processKeypress(CMD_COPY);
	destroyBuffer(scratch);
	E.buf = NULL;
	E.headbuf = NULL;
	E.windows[0]->buf = NULL;
}

/* ---- Grouping ---- */

void test_typing_a_word_is_one_record(void) {
	struct buffer *buf = loadedBuffer("Z");

	type_str("hello");
	assertContent(buf, "helloZ\n");

	/* One undo takes the whole word, and there is nothing behind it. */
	undo();
	assertContent(buf, "Z\n");
	undo();
	assertContent(buf, "Z\n");
}

/* The exception, stated rather than stumbled over.  Going from no
 * lines to one line, or typing onto the terminator row, genuinely adds
 * the newline as well as the character, so the first keystroke is its
 * own undo step and the run starts from the second.  The cost is one
 * extra step per burst that begins at the end of the buffer, not one
 * per word. */
void test_typing_at_end_of_buffer_costs_one_record(void) {
	struct buffer *buf = make_test_buffer("");

	type_str("hello");
	assertContent(buf, "hello\n");

	undo();
	assertContent(buf, "h\n");
	undo();
	assertContent(buf, "");
	undo();
	assertContent(buf, "");
}

void test_backspace_run_is_one_record(void) {
	struct buffer *buf = make_test_buffer("abcdef");
	buf->cx = 6;

	press_times(CMD_BACKSPACE, 3);
	assertContent(buf, "abc\n");

	undo();
	assertContent(buf, "abcdef\n");
}

void test_forward_delete_run_is_one_record(void) {
	struct buffer *buf = make_test_buffer("abcdef");
	buf->cx = 0;

	press_times(CMD_DELETE, 3);
	assertContent(buf, "def\n");

	undo();
	assertContent(buf, "abcdef\n");
}

/* Typing then deleting must not fold: the two are opposite operations
 * and one undo step for both would misstate both. */
void test_insert_then_delete_do_not_merge(void) {
	struct buffer *buf = loadedBuffer("Z");

	type_str("ab");
	processKeypress(CMD_BACKSPACE);
	assertContent(buf, "aZ\n");

	undo(); /* the backspace */
	assertContent(buf, "abZ\n");
	undo(); /* the typing */
	assertContent(buf, "Z\n");

	/* The same with C-d, and from the place where it would be
	 * easiest to get wrong: the deletion starts exactly where the
	 * typing started. */
	type_str("ab");
	processKeypress(CMD_HOME);
	processKeypress(CMD_DELETE);
	assertContent(buf, "bZ\n");

	undo(); /* the C-d */
	assertContent(buf, "abZ\n");
	undo(); /* the typing */
	assertContent(buf, "Z\n");
}

/* Typing away from the run's end starts a fresh step. */
void test_typing_elsewhere_starts_a_record(void) {
	struct buffer *buf = make_test_buffer("abcdef");
	buf->cx = 6;

	type_char('X');
	processKeypress(CMD_HOME);
	type_char('Y');
	assertContent(buf, "YabcdefX\n");

	undo(); /* only the Y */
	assertContent(buf, "abcdefX\n");
	undo(); /* only the X */
	assertContent(buf, "abcdef\n");
}

/* ---- The cap ---- */

/* Unbounded runs are correct but unrecoverable: one undo would discard
 * an arbitrarily long burst with no way to get part of it back.  A run
 * holds about twenty keystrokes, so 500 of them are about 25 steps. */
void test_long_run_is_capped(void) {
	struct buffer *buf = make_test_buffer("");

	for (int i = 0; i < 500; i++)
		type_char('x');
	TEST_ASSERT_EQUAL_INT(500, buf->row[0].size);

	int steps = undoStepsToStart(buf);
	TEST_ASSERT(steps >= 500 / 21);
	TEST_ASSERT(steps <= 500 / 20 + 2);
	assertContent(buf, "");
}

/* The cap counts operations, not bytes, so a run of three-byte
 * characters breaks in the same place a run of ASCII does.  Before
 * #104 the two paths disagreed by a factor of three. */
void test_cap_counts_operations_not_bytes(void) {
	struct buffer *ascii = make_test_buffer("");
	for (int i = 0; i < 100; i++)
		type_char('x');
	TEST_ASSERT_EQUAL_INT(100, ascii->row[0].size);
	int ascii_steps = undoStepsToStart(ascii);
	assertContent(ascii, "");

	struct buffer *cjk = make_test_buffer("");
	cjk->next = ascii; /* keep both reachable for cleanup */
	/* One decoded character waiting, as readKey() leaves it. */
	E.nunicode = 3;
	E.unicode[0] = 0xE6;
	E.unicode[1] = 0x97;
	E.unicode[2] = 0xA5;
	press_times(CMD_UNICODE, 100);
	TEST_ASSERT_EQUAL_INT(300, cjk->row[0].size);
	int cjk_steps = undoStepsToStart(cjk);
	assertContent(cjk, "");

	TEST_ASSERT(ascii_steps > 1);
	TEST_ASSERT_EQUAL_INT(ascii_steps, cjk_steps);
}

/* An explicit prefix argument is one command.  C-u 100 x inserts a
 * hundred characters, but undoing it must take one keystroke, not
 * five: the cap chops up a run of separate commands, not a single
 * command that happens to repeat. */
void test_prefix_argument_repeat_is_one_record(void) {
	struct buffer *buf = loadedBuffer("Z");

	E.self_insert_key = 'x';
	press_with_arg(100, CMD_SELF_INSERT);
	TEST_ASSERT_EQUAL_INT(101, buf->row[0].size);

	undo();
	assertContent(buf, "Z\n");
}

void test_prefix_argument_newlines_are_one_record(void) {
	struct buffer *buf = make_test_buffer("ab");
	buf->cx = 1;

	press_with_arg(30, CMD_NEWLINE);
	TEST_ASSERT_EQUAL_INT(32, buf->numrows);

	undo();
	assertContent(buf, "ab\n");
}

/* ---- Boundaries a run must not cross ---- */

/* A bulk edit stands alone.  Typing after one could find the earlier
 * typing still lined up with it and fold back across the bulk edit;
 * the yank here is the bulk edit. */
void test_run_does_not_fold_across_bulk_edit(void) {
	copyToKillRing("ZZ");
	struct buffer *buf = make_test_buffer("abc");
	buf->cx = 3;

	type_char('X');
	assertContent(buf, "abcX\n");
	buf->cx = 3;
	processKeypress(CMD_YANK);
	assertContent(buf, "abcZZX\n");
	processKeypress(CMD_END);
	type_char('Y');
	assertContent(buf, "abcZZXY\n");

	undo(); /* the Y */
	assertContent(buf, "abcZZX\n");
	undo(); /* the yank */
	assertContent(buf, "abcX\n");
	undo(); /* the X */
	assertContent(buf, "abc\n");
}

/* Undo closes the run.  Typing straight afterwards can land lined up
 * with the step undo just exposed; folding into it would be correct
 * but makes "a run is one uninterrupted burst" stop holding. */
void test_undo_closes_the_run(void) {
	struct buffer *buf = loadedBuffer("Z");

	/* Two steps: "ab" at the start, then "X" at the end. */
	type_str("ab");
	processKeypress(CMD_END);
	type_char('X');
	assertContent(buf, "abZX\n");

	/* Undo the X.  The step now on top is the "ab" run. */
	undo();
	assertContent(buf, "abZ\n");

	/* Type exactly where that run left off. */
	buf->cx = 2;
	type_char('c');
	assertContent(buf, "abcZ\n");

	/* The c is a step of its own: undo takes it and leaves "ab". */
	undo();
	assertContent(buf, "abZ\n");
	undo();
	assertContent(buf, "Z\n");
}

/* Redo re-exposes a step that may still have been open when it was
 * undone.  Typing after a redo must start a fresh one. */
void test_redo_closes_the_run(void) {
	struct buffer *buf = loadedBuffer("Z");

	type_str("ab");
	undo();
	redo();
	assertContent(buf, "abZ\n");

	buf->cx = 2;
	type_char('c');
	assertContent(buf, "abcZ\n");

	undo(); /* only the c */
	assertContent(buf, "abZ\n");
	undo();
	assertContent(buf, "Z\n");
}

/* ---- Newlines ---- */

/* RET is an ordinary insertion of the row separator, so it joins the
 * run rather than breaking it, and one undo takes the whole burst. */
void test_newline_joins_the_run(void) {
	struct buffer *buf = loadedBuffer("Z");

	type_char('a');
	processKeypress(CMD_NEWLINE);
	type_char('b');
	assertContent(buf, "a\nbZ\n");

	undo();
	assertContent(buf, "Z\n");
}

/* ---- Final-newline invariant, maintenance half ----
 *
 * Load establishes it; editing keeps it.  Two behaviours: a deletion
 * of nothing but the final newline is refused outright, and a deletion
 * that takes it along with real text is allowed and the terminator
 * re-appended in the same undo step. */

/* Refused, and refused before anything changes: the text is the same
 * and the buffer is not marked modified.  Doing it as a delete
 * followed by a repair would leave the buffer byte-identical but
 * spuriously modified. */
void test_deleting_only_the_final_newline_is_refused(void) {
	struct buffer *buf = make_test_buffer("abc");
	buf->cx = 3;
	buf->cy = 0;

	processKeypress(CMD_DELETE);

	assertContent(buf, "abc\n");
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	/* Nothing was done, so there is nothing to undo. */
	undo();
	assertContent(buf, "abc\n");
}

/* The same refusal reached from the other side.  Backspace at the
 * start of the terminator row asks to delete exactly the same byte. */
void test_backspace_onto_the_terminator_row_is_refused(void) {
	struct buffer *buf = make_test_buffer("abc");
	buf->cx = 0;
	buf->cy = 1;

	processKeypress(CMD_BACKSPACE);

	assertContent(buf, "abc\n");
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	undo();
	assertContent(buf, "abc\n");
}

/* Taking the final newline along with real text is a real edit, so it
 * proceeds; the terminator is re-appended.  One undo takes the whole
 * thing rather than exposing an intermediate state that never
 * existed. */
void test_deleting_past_the_end_repairs_in_one_step(void) {
	const char *lines[] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	assertContent(buf, "alpha\nbeta\n");

	/* C-w from inside the last line of text through the terminator. */
	buf->markx = 2;
	buf->marky = 1;
	buf->mark_active = 1;
	buf->cx = 0;
	buf->cy = 2;
	processKeypress(CMD_KILL_REGION);

	assertContent(buf, "alpha\nbe\n");

	undo();
	assertContent(buf, "alpha\nbeta\n");
}

/* The refusal is narrower than "the target is the final newline": it
 * applies only where the repair would undo the delete.  An empty
 * penultimate row leaves the invariant holding, so removing a trailing
 * blank line is a real edit. */
void test_backspace_deletes_a_trailing_blank_line(void) {
	const char *lines[] = { "a", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	assertContent(buf, "a\n\n");

	buf->cx = 0;
	buf->cy = 2;
	processKeypress(CMD_BACKSPACE);

	assertContent(buf, "a\n");
	TEST_ASSERT_EQUAL_INT(1, buf->dirty);

	undo();
	assertContent(buf, "a\n\n");
}

/* The same edit reached with C-d from the end of the row above. */
void test_delete_forward_removes_a_trailing_blank_line(void) {
	const char *lines[] = { "a", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);

	buf->cx = 0;
	buf->cy = 1;
	processKeypress(CMD_DELETE);

	assertContent(buf, "a\n");
	undo();
	assertContent(buf, "a\n\n");
}

/* A buffer holding only a newline empties: no text at all satisfies
 * the invariant too. */
void test_backspace_on_lone_blank_line_empties_the_buffer(void) {
	const char *lines[] = { "" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	assertContent(buf, "\n");

	buf->cx = 0;
	buf->cy = 1;
	processKeypress(CMD_BACKSPACE);

	assertContent(buf, "");

	undo();
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

	for (int i = 0; i < 57; i++)
		type_char('q');
	processKeypress(CMD_NEWLINE);
	for (int i = 0; i < 30; i++)
		type_char('w');
	press_times(CMD_BACKSPACE, 25);

	/* 57 q's end the first line; five of the w's are left. */
	TEST_ASSERT_EQUAL_INT(62, buf->row[0].size);
	TEST_ASSERT_EQUAL_STRING("wwwww", row_str(buf, 1));

	int steps = undoStepsToStart(buf);
	TEST_ASSERT(steps > 1);
	assertContent(buf, "alpha\nbeta\n");
}

/* ---- input bursts (#126) --------------------------------------- */

/* Undoing a burst run restores the buffer exactly. */
void test_burst_run_undoes_whole(void) {
	struct buffer *buf = loadedBuffer("keep");
	buf->cx = 4;
	E.input_burst = 1;
	for (int i = 0; i < 50; i++)
		selfInsert(buf, 'z', 1);
	E.input_burst = 0;
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("keep", (char *)buf->row[0].chars);
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

	RUN_TEST(test_mixed_burst_round_trips);
	RUN_TEST(test_burst_run_undoes_whole);

	return TEST_END();
}
