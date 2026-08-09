/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_undo.c: Undo/redo stack, coalescing, bulk replay.
 * Highest-value test target: undo bugs silently corrupt files. */

#include "test.h"
#include "completion.h"
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


/* ------------------------------------------------------------------
 * #102 — undo records for insertions at the virtual EOF line.
 *
 * A buffer of N rows represents the byte string
 *
 *     T = row[0] "\n" row[1] "\n" ... row[N-1] "\n"
 *
 * so it cannot represent text that does not end in "\n".  Inserting D
 * at the virtual EOF line (0, N) therefore really changes len(D) + 1
 * bytes.  Every insertion path used to record only len(D), so undo left
 * the materialising newline behind (M-> RET C-_ grew the buffer by a
 * blank line, permanently, and silently marked the buffer clean) and
 * redo re-inserted it (one RET redone as two).
 *
 * The records are now anchored to the end of the last real row, which
 * states the whole change.  The invariant these tests pin is
 *
 *     bulkInsert(B, R) == A     and     bulkDelete(A, R) == B
 *
 * checked on buffer CONTENT rather than row counts, since row counts
 * are what the old records got wrong.
 * ------------------------------------------------------------------ */

/* The byte string the buffer would be written out as.  save() applies
 * no policy of its own: the buffer already ends in a newline, so plain
 * serialisation is the whole of it.  NUL-terminated, because the
 * comparisons below use string equality. */
static char *contentOf(struct buffer *buf) {
	size_t len;
	return rowsToString(buf, &len);
}

static void undoAll(struct buffer *buf) {
	for (int i = 0; i < 64 && buf->undo != NULL; i++)
		processKeypress(CMD_UNDO);
}

static void redoAll(struct buffer *buf) {
	for (int i = 0; i < 64 && buf->redo != NULL; i++)
		processKeypress(CMD_REDO);
}

static struct buffer *eofTestBuffer(void) {
	static const char *lines[2] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	return buf;
}

/* Put the cursor at the end of the buffer, the way M-> does.  Under
 * cy < numrows that is the end of the last real row, not a virtual line
 * past it: the trailing newline is a real row, so the position is the
 * same place in the text and the row array can name it. */
static void gotoVirtualEOF(struct buffer *buf) {
	processKeypress(CMD_END_OF_FILE);
	TEST_ASSERT_EQUAL(buf->numrows - 1, buf->cy);
	TEST_ASSERT_EQUAL(buf->row[buf->cy].size, buf->cx);
}

static void setKillText(const char *s) {
	clearText(&E.kill);
	E.kill.str = (uint8_t *)xstrdup(s);
}

/* Drive one insertion path.  Kept as an enum rather than function
 * pointers so a failure names the path. */
enum eof_op {
	OP_RET,
	OP_OPEN_LINE,
	OP_NEWLINE_INDENT,
	OP_SELF_INSERT,
	OP_TAB,
	OP_YANK
};

static void runEofOp(enum eof_op op) {
	switch (op) {
	case OP_RET:
		processKeypress(CMD_NEWLINE);
		break;
	case OP_OPEN_LINE:
		processKeypress(CMD_OPEN_LINE);
		break;
	case OP_NEWLINE_INDENT:
		processKeypress(CMD_NEWLINE_INDENT);
		break;
	case OP_SELF_INSERT:
		E.self_insert_key = 'x';
		processKeypress(CMD_SELF_INSERT);
		break;
	case OP_TAB:
		processKeypress(CMD_TAB);
		break;
	case OP_YANK:
		setKillText("hello");
		processKeypress(CMD_YANK);
		break;
	}
}

/* Round-trip matrix, one case per insertion path: undo restores the
 * original, redo restores the edit, undo again restores the original.
 *
 * This used to take an at_virtual_eof flag and run each path a second
 * time as a control, positioned by hand at the end of the last real
 * row.  Since the trailing newline became a real row, gotoVirtualEOF
 * lands on exactly that position, so the control ran the identical
 * scenario and asserted nothing the case above it had not. */
static void eofRoundTrip(enum eof_op op) {
	cleanupTestEditor();
	initTestEditor();
	struct buffer *buf = eofTestBuffer();

	gotoVirtualEOF(buf);

	char *original = contentOf(buf);
	runEofOp(op);
	char *edited = contentOf(buf);

	/* The operation must actually have done something, or the
	 * round-trip below would pass vacuously. */
	TEST_ASSERT(strcmp(original, edited) != 0);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	redoAll(buf);
	char *redone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(edited, redone);

	undoAll(buf);
	char *undone2 = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone2);

	free(original);
	free(edited);
	free(undone);
	free(redone);
	free(undone2);
	clearText(&E.kill);
	cleanupTestEditor();
}

void test_eof_roundtrip_newline(void) {
	eofRoundTrip(OP_RET);
}

void test_eof_roundtrip_open_line(void) {
	eofRoundTrip(OP_OPEN_LINE);
}

void test_eof_roundtrip_newline_indent(void) {
	eofRoundTrip(OP_NEWLINE_INDENT);
}

void test_eof_roundtrip_self_insert(void) {
	eofRoundTrip(OP_SELF_INSERT);
}

void test_eof_roundtrip_tab(void) {
	eofRoundTrip(OP_TAB);
}

void test_eof_roundtrip_yank(void) {
	eofRoundTrip(OP_YANK);
}

/* §1 — the original symptom.  Five repetitions of M-> RET C-_ used to
 * grow a 2-row buffer to 7 rows, one leaked blank line at a time. */
void test_eof_repeated_newline_undo_does_not_accumulate(void) {
	struct buffer *buf = eofTestBuffer();
	char *original = contentOf(buf);

	for (int i = 0; i < 5; i++) {
		gotoVirtualEOF(buf);
		processKeypress(CMD_NEWLINE);
		processKeypress(CMD_UNDO);
	}

	char *after = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, after);
	TEST_ASSERT_EQUAL(3, buf->numrows);

	free(original);
	free(after);
}

/* §1 — redo over-inserted: RET at EOF, undo, redo gave two blank lines
 * where the original operation produced one. */
void test_eof_newline_redo_inserts_one_row_not_two(void) {
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);

	processKeypress(CMD_NEWLINE);
	TEST_ASSERT_EQUAL(4, buf->numrows);
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL(3, buf->numrows);
	processKeypress(CMD_REDO);
	TEST_ASSERT_EQUAL(4, buf->numrows);

}

/* §9.2 — multi-row payloads through the mutate path: line counts,
 * leading and trailing newlines, newlines only, and multi-byte text. */
static void eofYankRoundTrip(const char *payload, const char *expect) {
	cleanupTestEditor();
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);

	char *original = contentOf(buf);
	setKillText(payload);
	processKeypress(CMD_YANK);

	char *edited = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(expect, edited);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	redoAll(buf);
	char *redone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(edited, redone);

	free(original);
	free(edited);
	free(undone);
	free(redone);
	clearText(&E.kill);
	cleanupTestEditor();
}

/* #105 acceptance (§10).  The mirror of the proof that motivated this
 * task.
 *
 * Before #105, a hand-built undo record naming the insertion's own
 * logical coordinates could not be replayed: an insertion at the end of
 * a two-row buffer produced the record (0,2)->(0,3), and bulkDelete
 * could not address row 3 because row 3 was not in the array.  The
 * guard in bulkDelete returned having deleted nothing, undo left the
 * row behind, and anchorInsert existed to translate such positions onto
 * a row that did exist.
 *
 * The defect was in the representation's inability to name the
 * position, not in the record.  With the trailing newline held as a
 * real row, logical and addressable coordinates are the same thing, so
 * the record that used to be unreplayable is now the only form there
 * is.  Same construction as the original proof, expectation inverted.
 *
 * Built by hand rather than through an editing command on purpose: this
 * asserts that the representation admits the record, independently of
 * whatever coordinates the mutation layer happens to produce. */
void test_logical_insert_record_round_trips(void) {
	const char *lines[] = { "a", "b" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	E.buf = buf;
	clearUndosAndRedos(buf);

	char *before = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING("a\nb\n", before);
	int rows_before = buf->numrows;

	/* The record names where the edit happened: (0,2) -> (0,3). */
	struct undo *u = newUndo();
	u->startx = 0;
	u->starty = 2;
	u->endx = 0;
	u->endy = 3;
	u->data[0] = '\n';
	u->datalen = 1;
	u->delete = 0;
	pushUndo(buf, u);

	bulkInsert(buf, 0, 2, (const uint8_t *)"\n", 1);
	TEST_ASSERT_EQUAL_INT(rows_before + 1, buf->numrows);

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_INT(rows_before, buf->numrows);
	char *after = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(before, after);

	doRedo(buf, 1);
	TEST_ASSERT_EQUAL_INT(rows_before + 1, buf->numrows);

	free(before);
	free(after);
}

/* The eof_payload_* family below asserts on serialised content rather
 * than on record coordinates.  Coordinates were the wrong thing to pin:
 * they described anchorInsert, a compensation #105 removed. */

void test_eof_payload_one_line(void) {
	eofYankRoundTrip("one", "alpha\nbeta\none\n");
}

void test_eof_payload_two_lines(void) {
	eofYankRoundTrip("one\ntwo", "alpha\nbeta\none\ntwo\n");
}

void test_eof_payload_leading_newline(void) {
	eofYankRoundTrip("\none", "alpha\nbeta\n\none\n");
}

void test_eof_payload_newlines_only(void) {
	eofYankRoundTrip("\n\n", "alpha\nbeta\n\n\n");
}

void test_eof_payload_cjk(void) {
	eofYankRoundTrip("\xe6\x97\xa5\xe6\x9c\xac\n\xe8\xaa\x9e",
			 "alpha\nbeta\n\xe6\x97\xa5\xe6\x9c\xac\n\xe8\xaa\x9e\n");
}

/* §9.3 — a payload ending in "\n" at the virtual EOF: the divergence
 * that made bulkInsert and the typed paths disagree.  bulkInsert used
 * to add a row the typed path never would.  The row array supplies the
 * final terminator, so "one\n" and "one" land identically here — this
 * is the one bounded, user-visible behaviour change. */
void test_eof_payload_trailing_newline_adds_no_extra_row(void) {
	eofYankRoundTrip("one\n", "alpha\nbeta\none\n");

	cleanupTestEditor();
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	setKillText("one\n");
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL(4, buf->numrows);
	clearText(&E.kill);
}

/* §9.5 — the regression guard for mutateReplace.  The anchor lands at
 * the end of the last row, which is exactly where a mark set with
 * C-SPC at end of buffer sits, and adjustPoint's insert branch treats a
 * point exactly at startx as being after the insertion.  So if the
 * point adjustment is fed the anchored range instead of the logical
 * one, this mark silently moves.  Nothing else in the plan catches it. */
void test_mark_unmoved_by_newline_at_virtual_eof(void) {
	struct buffer *buf = eofTestBuffer();
	/* Mark at the end of the last row of text -- "beta", row 1.  That
	 * is the position the anchor used to map onto, and where
	 * adjustPoint's insert branch is most likely to move a point it
	 * should leave alone.  Written as an explicit row rather than
	 * numrows - 1, which since #105 names the trailing empty row and
	 * would put the mark somewhere else entirely. */
	buf->cy = 1;
	buf->cx = buf->row[1].size;
	processKeypress(CMD_SET_MARK);
	int markx = buf->markx, marky = buf->marky;
	TEST_ASSERT_EQUAL(4, markx);
	TEST_ASSERT_EQUAL(1, marky);

	gotoVirtualEOF(buf);
	processKeypress(CMD_NEWLINE);

	TEST_ASSERT_EQUAL(markx, buf->markx);
	TEST_ASSERT_EQUAL(marky, buf->marky);

}

/* Same guard on the mutate path, where the hazard actually lives:
 * bulkInsert adjusts points internally from whatever range it is
 * handed, so anchoring the mutation must not anchor the adjustment. */
void test_mark_unmoved_by_yank_at_virtual_eof(void) {
	struct buffer *buf = eofTestBuffer();
	/* Mark at the end of the last row of text -- "beta", row 1.  That
	 * is the position the anchor used to map onto, and where
	 * adjustPoint's insert branch is most likely to move a point it
	 * should leave alone.  Written as an explicit row rather than
	 * numrows - 1, which since #105 names the trailing empty row and
	 * would put the mark somewhere else entirely. */
	buf->cy = 1;
	buf->cx = buf->row[1].size;
	processKeypress(CMD_SET_MARK);
	int markx = buf->markx, marky = buf->marky;

	/* End of buffer.  cy == numrows is unreachable under #105, so
	 * this is the last real row -- the same place in the text. */
	buf->cy = buf->numrows - 1;
	buf->cx = buf->row[buf->cy].size;
	setKillText("\n");
	processKeypress(CMD_YANK);

	TEST_ASSERT_EQUAL(markx, buf->markx);
	TEST_ASSERT_EQUAL(marky, buf->marky);

	clearText(&E.kill);
}

/* §9.6 — insertNewlineAndIndent calls undoAppendChar again for the
 * indent bytes after the newline has already moved the cursor, so the
 * compound path exercises the fresh-record and continuation branches in
 * one operation. */
void test_newline_and_indent_at_virtual_eof(void) {
	static const char *lines[2] = { "alpha", "\t  beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	gotoVirtualEOF(buf);

	char *original = contentOf(buf);
	processKeypress(CMD_NEWLINE_INDENT);
	char *edited = contentOf(buf);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	redoAll(buf);
	char *redone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(edited, redone);

	free(original);
	free(edited);
	free(undone);
	free(redone);
}

/* §9.7 — repeat counts.  C-u 5 <char> takes undoSelfInsert's count > 1
 * branch, which builds a whole record in one go rather than appending. */
void test_repeat_count_self_insert_at_virtual_eof(void) {
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	char *original = contentOf(buf);

	E.uarg = 5;
	E.self_insert_key = 'q';
	processKeypress(CMD_SELF_INSERT);

	char *edited = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING("alpha\nbeta\nqqqqq\n", edited);
	free(edited);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	free(original);
	free(undone);
}

void test_repeat_count_newline_at_virtual_eof(void) {
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	char *original = contentOf(buf);

	E.uarg = 5;
	processKeypress(CMD_NEWLINE);
	TEST_ASSERT_EQUAL(8, buf->numrows);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);
	TEST_ASSERT_EQUAL(3, buf->numrows);

	free(original);
	free(undone);
}

/* §10 — chain_to_prev.  yankRectangle combines mutateExtendRows with a
 * chained mutateReplace; the two must not both account for the same
 * newline.  mutateExtendRows runs first and makes the target a real
 * row, so anchorInsert is the identity inside the replace. */
void test_rectangle_yank_at_virtual_eof_roundtrip(void) {
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	char *original = contentOf(buf);

	clearText(&E.kill);
	E.kill.str = (uint8_t *)xstrdup("abcd"); /* 2 wide, 2 high, flat */
	E.kill.is_rectangle = 1;
	E.kill.rect_width = 2;
	E.kill.rect_height = 2;
	processKeypress(CMD_YANK);

	char *edited = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING("alpha\nbeta\nab\ncd\n", edited);

	/* The head record is the final-newline repair: the rectangle's
	 * bottom row became the last line of the file and needed its
	 * terminator, recorded paired so it undoes with the yank rather
	 * than as a step of its own.  The replace's insert record sits
	 * beneath it, and is unanchored -- the extension already
	 * accounted for both newlines. */
	TEST_ASSERT_EQUAL(1, buf->undo->paired);
	TEST_ASSERT_EQUAL(0, buf->undo->prev->startx);
	TEST_ASSERT_EQUAL(2, buf->undo->prev->starty);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	redoAll(buf);
	char *redone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(edited, redone);

	free(original);
	free(edited);
	free(undone);
	free(redone);
	clearText(&E.kill);
}


/* Defect: the redo chain had no exit.  After C-/ left more to redo,
 * emil entered a mode where C-_ continued redoing -- but the mode was
 * never cleared when the redo stack emptied, so every later undo
 * keystroke was swallowed as a redo of nothing and undo appeared dead
 * until some unrelated key was pressed.  Found by fuzz_undo.c as
 * "kill-para (uarg 6), undo (uarg 5), redo" failing to restore. */
void test_redo_chain_releases_undo(void) {
	const char *lines[] = { "alpha beta", "(gamma delta).", "  indented",
				"", "epsilon" };
	struct buffer *buf = make_test_buffer_lines(lines, 5);
	E.buf = buf;
	clearUndosAndRedos(buf);

	E.uarg = 6;
	processKeypress(CMD_KILL_PARA);
	E.uarg = 5;
	processKeypress(CMD_UNDO);
	E.uarg = 0;
	processKeypress(CMD_REDO);

	/* Undo must reach the original content rather than stalling. */
	for (int k = 0; k < 64 && buf->undo != NULL; k++) {
		E.uarg = 0;
		processKeypress(CMD_UNDO);
	}

	TEST_ASSERT_NULL(buf->undo);
	TEST_ASSERT_EQUAL_INT(6, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("alpha beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("epsilon", row_str(buf, 4));
}

/* A prompt reset rebuilds the minibuffer rows outside the mutation
 * layer, so records from the previous session describe text that is
 * gone.  Replaying one made bulkDelete compute a negative length. */
void test_minibuffer_reset_drops_stale_undo_records(void) {
	makeMinibuffer();
	E.buf = E.minibuf;

	/* First prompt session: type into the minibuffer, which records
	 * undo against it through the ordinary edit path -- which is how
	 * a prompt records, since its loop dispatches typing through
	 * processKeypress into selfInsert. */
	replaceMinibufferText(E.minibuf, "");
	selfInsert(E.minibuf, 'a', 1);
	selfInsert(E.minibuf, 'b', 1);
	selfInsert(E.minibuf, 'c', 1);
	TEST_ASSERT_NOT_NULL(E.minibuf->undo);

	/* Second prompt session: the reset must take the records with
	 * it, since they describe rows that no longer exist. */
	replaceMinibufferText(E.minibuf, "");
	TEST_ASSERT_NULL(E.minibuf->undo);
	TEST_ASSERT_NULL(E.minibuf->redo);

	/* Undo now has nothing to replay and must leave a sane row
	 * rather than a negative-length memmove. */
	doUndo(E.minibuf, 1);
	TEST_ASSERT_EQUAL_INT(1, E.minibuf->numrows);
	TEST_ASSERT(E.minibuf->row[0].size >= 0);

	freeMinibuffer();
}

/* Whatever a record claims, bulkDelete must not derive a negative
 * length from it. */
void test_bulk_delete_clamps_out_of_range_end(void) {
	struct buffer *buf = make_test_buffer("abc");
	E.buf = buf;

	/* endx far past the end of a 3-byte row, as a stale record
	 * would give. */
	bulkDelete(buf, 0, 0, 99, 0);

	TEST_ASSERT_EQUAL_INT(0, buf->row[0].size);
	TEST_ASSERT_EQUAL_STRING("", (char *)buf->row[0].chars);

}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_undo_insert_chars);
	RUN_TEST(test_undo_then_redo);
	RUN_TEST(test_multiple_sequential_undos);
	RUN_TEST(test_undo_delete_chars);


	RUN_TEST(test_undo_empty_stack);
	RUN_TEST(test_redo_cleared_after_new_edit);

	RUN_TEST(test_undo_newline_insert);
	RUN_TEST(test_quoted_newline_undo_preserves_following_row);
	RUN_TEST(test_backspace_at_origin_is_a_noop);

	RUN_TEST(test_mutate_replace_readonly);


	RUN_TEST(test_eof_roundtrip_newline);
	RUN_TEST(test_eof_roundtrip_open_line);
	RUN_TEST(test_eof_roundtrip_newline_indent);
	RUN_TEST(test_eof_roundtrip_self_insert);
	RUN_TEST(test_eof_roundtrip_tab);
	RUN_TEST(test_eof_roundtrip_yank);
	RUN_TEST(test_eof_repeated_newline_undo_does_not_accumulate);
	RUN_TEST(test_eof_newline_redo_inserts_one_row_not_two);
	RUN_TEST(test_logical_insert_record_round_trips);
	RUN_TEST(test_eof_payload_one_line);
	RUN_TEST(test_eof_payload_two_lines);
	RUN_TEST(test_eof_payload_leading_newline);
	RUN_TEST(test_eof_payload_newlines_only);
	RUN_TEST(test_eof_payload_cjk);
	RUN_TEST(test_eof_payload_trailing_newline_adds_no_extra_row);
	RUN_TEST(test_mark_unmoved_by_newline_at_virtual_eof);
	RUN_TEST(test_mark_unmoved_by_yank_at_virtual_eof);
	RUN_TEST(test_newline_and_indent_at_virtual_eof);
	RUN_TEST(test_repeat_count_self_insert_at_virtual_eof);
	RUN_TEST(test_repeat_count_newline_at_virtual_eof);
	RUN_TEST(test_rectangle_yank_at_virtual_eof_roundtrip);
	RUN_TEST(test_redo_chain_releases_undo);
	RUN_TEST(test_minibuffer_reset_drops_stale_undo_records);
	RUN_TEST(test_bulk_delete_clamps_out_of_range_end);
	return TEST_END();
}
