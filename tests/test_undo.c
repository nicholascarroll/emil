/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_undo.c: Undo/redo stack, coalescing, bulk replay.
 * Highest-value test target: undo bugs silently corrupt files.
 *
 * Two kinds of test live here.  Most issue commands through
 * processKeypress() and compare the text a save would write; those
 * survive any reshaping of undo's internals.  The tests of the
 * mutation layer's refusals and of replay against changed text
 * (test_mutate_*, test_*_reports_*, test_rebuild_*) call that layer
 * directly, because the states they need cannot be reached with
 * keys. */

#include "test.h"
#include "completion.h"
#include "test_harness.h"
#include "edit.h"
#include "mutate.h"
#include "undo.h"
#include "prompt.h"
#include "buffer.h"
#include "motion.h"
#include "transform.h"
#include "keymap.h"
#include <stdint.h>

/* For tests that make several buffers: the harness frees only the last. */
static void drop_test_buffer(struct buffer *buf) {
	destroyBuffer(buf);
	E.buf = NULL;
	E.headbuf = NULL;
	E.windows[0]->buf = NULL;
}

/* Put `text` on the kill ring the way a user would: select it in a
 * scratch buffer and copy it (M-w).  `text` may hold newlines.  The
 * scratch buffer is gone on return, so call this before making the
 * buffer under test. */
static void copyToKillRing(const char *text) {
	/* One scratch row per line of text. */
	char *copy = xstrdup(text);
	const char *lines[16];
	int n = 0;
	lines[n++] = copy;
	for (char *p = copy; *p; p++) {
		if (*p == '\n' && n < 16) {
			*p = 0;
			lines[n++] = p + 1;
		}
	}
	struct buffer *scratch = make_test_buffer_lines(lines, n);
	scratch->markx = 0;
	scratch->marky = 0;
	scratch->mark_active = 1;
	scratch->cy = n - 1;
	scratch->cx = scratch->row[n - 1].size;
	processKeypress(CMD_COPY);
	drop_test_buffer(scratch);
	free(copy);
}

/* ---- Truncated UTF-8 at the end of a row ---- */

/* C-d read and recorded bytes past the row, so undo added a NUL. */
void test_undo_delchar_truncated_lead(void) {
	const char *leads[] = { "a\xC3", "a\xE2", "a\xF0" };
	for (int i = 0; i < 3; i++) {
		const char *lines[] = { leads[i] };
		struct buffer *buf = make_test_buffer_lines(lines, 1);
		buf->cx = 1;
		processKeypress(CMD_DELETE);
		TEST_ASSERT_EQUAL_STRING("a", row_str(buf, 0));
		processKeypress(CMD_UNDO);
		TEST_ASSERT_EQUAL_STRING(leads[i], row_str(buf, 0));
		TEST_ASSERT_EQUAL_INT((int)strlen(leads[i]), buf->row[0].size);
		processKeypress(CMD_REDO);
		TEST_ASSERT_EQUAL_STRING("a", row_str(buf, 0));
		drop_test_buffer(buf);
	}
}

/* forwardWordEnd stepped past the row over a truncated sequence, so M-u
 * and friends asked to replace a range that doesn't exist. */
void test_word_transform_truncated_lead(void) {
	const char *lines[] = { "ab\xC3" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	buf->cx = 0;
	buf->cy = 0;
	processKeypress(CMD_UPCASE_WORD);
	TEST_ASSERT_EQUAL_STRING("AB\xC3", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(3, buf->row[0].size);
	/* Point is left inside the row, not past it. */
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT(buf->cx <= 3);
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("ab\xC3", row_str(buf, 0));
}

/* ---- Invalid ranges are refused ---- */

/* One record and one redo entry, so "nothing changed" means something. */
static struct buffer *make_invalid_range_buffer(void) {
	const char *lines[] = { "abc", "def" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	mutateInsert(buf, 0, 0, (const uint8_t *)"X", 1, NULL, NULL);
	mutateInsert(buf, 1, 0, (const uint8_t *)"Y", 1, NULL, NULL);
	doUndo(buf, 1);
	return buf;
}

static void assert_range_refused(int sx, int sy, int ex, int ey) {
	struct buffer *buf = make_invalid_range_buffer();
	int numrows = buf->numrows;
	struct undo *undo = buf->undo, *redo = buf->redo;
	buf->cx = 2;
	buf->cy = 1;
	int ox = -9, oy = -9;

	TEST_ASSERT_EQUAL_INT(MUTATE_INVALID,
			      mutateReplace(buf, sx, sy, ex, ey,
					    (const uint8_t *)"Z", 1, 0, &ox, &oy));
	TEST_ASSERT_EQUAL_INT(MUTATE_INVALID, mutateDelete(buf, sx, sy, ex, ey));
	TEST_ASSERT_EQUAL_INT(MUTATE_INVALID,
			      mutateDeleteChar(buf, sx, sy, ex, ey));
	TEST_ASSERT_EQUAL_INT(-1, bulkDelete(buf, sx, sy, ex, ey));

	TEST_ASSERT_EQUAL_STRING("Xabc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("def", row_str(buf, 1));
	TEST_ASSERT_EQUAL_INT(numrows, buf->numrows);
	TEST_ASSERT_TRUE(buf->undo == undo);
	TEST_ASSERT_TRUE(buf->redo == redo);
	/* A caller's "move to the end of the edit" stays put. */
	TEST_ASSERT_EQUAL_INT(2, ox);
	TEST_ASSERT_EQUAL_INT(1, oy);
	drop_test_buffer(buf);
}

void test_mutate_refuses_invalid_ranges(void) {
	assert_range_refused(1, 0, 9, 0);   /* end x past its row */
	assert_range_refused(1, 0, 9, 1);   /* end x past a later row */
	assert_range_refused(1, 0, 1, 7);   /* end row past the last */
	assert_range_refused(2, 0, 1, 0);   /* reversed on one row */
	assert_range_refused(1, 1, 1, 0);   /* reversed across rows */
	assert_range_refused(9, 0, 0, 1);   /* start x past its row */
	assert_range_refused(0, 7, 0, 8);   /* start row past the last */
	assert_range_refused(-1, 0, 1, 0);  /* negative */
	assert_range_refused(0, -1, 1, 0);
}

void test_mutate_refuses_invalid_insert(void) {
	struct buffer *buf = make_invalid_range_buffer();
	struct undo *undo = buf->undo;
	const uint8_t *x = (const uint8_t *)"x";
	TEST_ASSERT_EQUAL_INT(MUTATE_INVALID,
			      mutateInsert(buf, 9, 0, x, 1, NULL, NULL));
	TEST_ASSERT_EQUAL_INT(MUTATE_INVALID,
			      mutateInsert(buf, 0, 7, x, 1, NULL, NULL));
	TEST_ASSERT_EQUAL_INT(MUTATE_INVALID,
			      mutateInsertChar(buf, 9, 0, x, 1, NULL, NULL));
	TEST_ASSERT_EQUAL_INT(-1, bulkInsert(buf, 9, 0, x, 1));
	TEST_ASSERT_EQUAL_STRING("Xabc", row_str(buf, 0));
	TEST_ASSERT_TRUE(buf->undo == undo);
}

void test_mutate_extend_refuses_wrong_row_count(void) {
	struct buffer *buf = make_invalid_range_buffer();
	int numrows = buf->numrows;
	TEST_ASSERT_EQUAL_INT(MUTATE_INVALID, mutateExtendRows(buf, 1, 1));
	TEST_ASSERT_EQUAL_INT(numrows, buf->numrows);
	TEST_ASSERT_EQUAL_INT(MUTATE_DONE, mutateExtendRows(buf, numrows, 1));
	TEST_ASSERT_EQUAL_INT(numrows + 1, buf->numrows);
}

/* ---- Replay checks the record against the text ---- */

/* Text rebuilt outside the mutation layer, keeping its history, as a
 * writable special buffer's is. */
void test_undo_reports_stale_position(void) {
	struct buffer *buf = make_test_buffer("a long line of text");
	mutateInsert(buf, 15, 0, (const uint8_t *)"x", 1, NULL, NULL);
	bufferResetRows(buf);
	insertRow(buf, 0, (const uint8_t *)"short", 5);

	struct undo *undo = buf->undo;
	TEST_ASSERT_EQUAL_INT(UNDO_MISMATCH, doUndo(buf, 1));
	TEST_ASSERT_EQUAL_STRING("short", row_str(buf, 0));
	TEST_ASSERT_TRUE(buf->undo == undo);
	TEST_ASSERT_TRUE(buf->redo == NULL);
}

/* A record's range still exists, but holds other text: a change made
 * without a record, as TAB in a prompt does. */
void test_undo_reports_changed_text(void) {
	struct buffer *buf = make_test_buffer("");
	mutateInsert(buf, 0, 0, (const uint8_t *)"ab", 2, NULL, NULL);
	buf->cx = 0;
	buf->cy = 0;
	/* Rebuild the row by hand: nothing in emil changes text this way. */
	bufferResetRows(buf);
	insertRow(buf, 0, (const uint8_t *)"\tab", 3);
	buf->cx = 1;

	struct undo *undo = buf->undo;
	TEST_ASSERT_EQUAL_INT(UNDO_MISMATCH, doUndo(buf, 1));
	TEST_ASSERT_EQUAL_STRING("\tab", row_str(buf, 0));
	TEST_ASSERT_TRUE(buf->undo == undo);
}

/* TAB in a prompt is an ordinary edit, so undo in the prompt is exact.
 * It used to make no record, so undo deleted the wrong bytes. */
void test_prompt_tab_undoes(void) {
	makeMinibuffer();
	make_test_buffer("x");
	int keys[] = { 'a', 'b', KEY_CTRL('a'), '\t', KEY_CTRL('_'), '\r' };
	scriptKeys(keys, 6);
	muteStdout();
	uint8_t *got = editorPrompt("p: ", PROMPT_PLAIN, NULL);
	unmuteStdout();
	clearKeys();
	TEST_ASSERT_EQUAL_STRING("ab", (char *)got);
	free(got);
	freeMinibuffer();
}

/* A rebuilt buffer with no history just gets its new rows. */
void test_rebuild_without_history_no_record(void) {
	struct buffer *buf = make_test_buffer("old");
	buf->read_only = 1;
	struct mutateRebuild r = mutateRebuildBegin(buf);
	insertRow(buf, 0, (const uint8_t *)"new", 3);
	mutateRebuildEnd(buf, &r);
	TEST_ASSERT_EQUAL_STRING("new", row_str(buf, 0));
	TEST_ASSERT_TRUE(buf->undo == NULL);
	TEST_ASSERT_EQUAL_INT(1, buf->read_only);
}

/* With history, the rebuild is an ordinary replace: undoing it gets the
 * old text back, and the earlier history still works. */
void test_rebuild_with_history_is_an_edit(void) {
	const char *lines[] = { "a long line of text", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	mutateInsert(buf, 15, 0, (const uint8_t *)"x", 1, NULL, NULL);

	struct mutateRebuild r = mutateRebuildBegin(buf);
	buf->read_only = 1;
	insertRow(buf, 0, (const uint8_t *)"short", 5);
	insertRow(buf, 1, (const uint8_t *)"", 0);
	mutateRebuildEnd(buf, &r);

	TEST_ASSERT_EQUAL_STRING("short", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_INT(1, buf->read_only);

	buf->read_only = 0;
	TEST_ASSERT_EQUAL_INT(UNDO_DONE, doUndo(buf, 1));
	TEST_ASSERT_EQUAL_STRING("a long line of xtext", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(UNDO_DONE, doUndo(buf, 1));
	TEST_ASSERT_EQUAL_STRING("a long line of text", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(UNDO_DONE, doRedo(buf, 2));
	TEST_ASSERT_EQUAL_STRING("short", row_str(buf, 0));
}

/* A stale redo entry is reported in the same way. */
void test_redo_reports_changed_text(void) {
	struct buffer *buf = make_test_buffer("abc");
	mutateDelete(buf, 0, 0, 1, 0);
	doUndo(buf, 1);
	bufferResetRows(buf);
	insertRow(buf, 0, (const uint8_t *)"xyz", 3);

	struct undo *redo = buf->redo;
	TEST_ASSERT_EQUAL_INT(UNDO_MISMATCH, doRedo(buf, 1));
	TEST_ASSERT_EQUAL_STRING("xyz", row_str(buf, 0));
	TEST_ASSERT_TRUE(buf->redo == redo);
}

/* ---- Basic undo/redo ---- */

static void typeChar(int ch) {
	E.self_insert_key = ch;
	processKeypress(CMD_SELF_INSERT);
}

/* A new edit after an undo discards what could have been redone. */
void test_redo_cleared_after_new_edit(void) {
	struct buffer *buf = make_test_buffer("A");
	buf->cx = 1;

	typeChar('B');
	TEST_ASSERT_EQUAL_STRING("AB", row_str(buf, 0));

	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("A", row_str(buf, 0));

	buf->cx = 1;
	typeChar('C');
	TEST_ASSERT_EQUAL_STRING("AC", row_str(buf, 0));

	/* The B is gone for good: redo has nothing to bring back. */
	processKeypress(CMD_REDO);
	TEST_ASSERT_EQUAL_STRING("AC", row_str(buf, 0));
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
	mutateReplace(buf, 0, 0, 5, 0,
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
 * below. */

void test_quoted_newline_undo_preserves_following_row(void) {
	const char *lines[] = { "abcdef", "ghi" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 3;
	buf->cy = 0;

	/* C-q C-j */
	int keys[] = { KEY_CTRL('j') };
	scriptKeys(keys, 1);
	processKeypress(CMD_QUOTED_INSERT);
	TEST_ASSERT_EQUAL_INT(1, test_key_pos);
	clearKeys();

	TEST_ASSERT_EQUAL_INT(4, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("def", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("ghi", row_str(buf, 2));

	/* Previously produced a single row, "abcghi". */
	processKeypress(CMD_UNDO);
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

	processKeypress(CMD_BACKSPACE);

	TEST_ASSERT_EQUAL_STRING("ab", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);

	/* Nothing was done, so there is nothing to undo. */
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("ab", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
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

/* Drive one insertion path.  Kept as an enum rather than function
 * pointers so a failure names the path. */
enum eof_op {
	OP_RET,
	OP_OPEN_LINE,
	OP_NEWLINE_INDENT,
	OP_SELF_INSERT,
	OP_TAB
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
	copyToKillRing(payload);
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);

	char *original = contentOf(buf);
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
	cleanupTestEditor();
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
	copyToKillRing("one\n");
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL(4, buf->numrows);
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
	copyToKillRing("\n");
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
	TEST_ASSERT_EQUAL(4, buf->markx);
	TEST_ASSERT_EQUAL(1, buf->marky);

	/* End of buffer.  cy == numrows is unreachable under #105, so
	 * this is the last real row -- the same place in the text. */
	buf->cy = buf->numrows - 1;
	buf->cx = buf->row[buf->cy].size;
	processKeypress(CMD_YANK);

	/* Yank pushes the old mark onto the ring (as Emacs's push-mark
	 * does) and sets the mark where the text went in; the pushed
	 * entry is the point this guard watches.  C-u C-SPC walks back
	 * through them: first to where the text went in, then to the
	 * old mark, which must still be at the end of "beta". */
	E.uarg = 4;
	processKeypress(CMD_SET_MARK);
	TEST_ASSERT_EQUAL(0, buf->cx);
	TEST_ASSERT_EQUAL(2, buf->cy);
	E.uarg = 4;
	processKeypress(CMD_SET_MARK);
	TEST_ASSERT_EQUAL(4, buf->cx);
	TEST_ASSERT_EQUAL(1, buf->cy);
}

/* Spec 05 observation O1: as in Emacs, C-y sets the mark at the start
 * of the yanked text (pushing the old one), so C-w right after C-y
 * kills exactly what was yanked; M-y moves that mark to the new text
 * without pushing again. */
void test_yank_sets_mark_at_start_of_yanked_text(void) {
	copyToKillRing("p\nq");
	const char *lines[] = { "xy" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	buf->markx = 2;
	buf->marky = 0;
	buf->cx = 1;
	buf->cy = 0;
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("xp", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("qy", row_str(buf, 1));
	TEST_ASSERT_EQUAL_INT(1, buf->markx);
	TEST_ASSERT_EQUAL_INT(0, buf->marky);
	TEST_ASSERT_EQUAL_INT(0, buf->mark_active);

	/* C-w straight after C-y kills exactly what was yanked... */
	processKeypress(CMD_KILL_REGION);
	TEST_ASSERT_EQUAL_STRING("xy", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);

	/* ...so yanking again puts the same text back. */
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("xp", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("qy", row_str(buf, 1));
}

/* The other half of O1: the mark the yank replaced was pushed, not
 * lost, so C-u C-SPC can still get back to it. */
void test_yank_pushes_the_old_mark(void) {
	copyToKillRing("p\nq");
	const char *lines[] = { "xy" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	buf->markx = 2;
	buf->marky = 0;
	buf->cx = 1;
	buf->cy = 0;
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("xp", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("qy", row_str(buf, 1));

	/* First pop: to the mark the yank set, the start of the text. */
	E.uarg = 4;
	processKeypress(CMD_SET_MARK);
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);

	/* Second pop: to the old mark.  It was after the "y", and the
	 * "y" is now on the second row, after the "q". */
	E.uarg = 4;
	processKeypress(CMD_SET_MARK);
	TEST_ASSERT_EQUAL_INT(2, buf->cx);
	TEST_ASSERT_EQUAL_INT(1, buf->cy);
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

/* §10 — a rectangle yanked at the end of the buffer has to add rows
 * for itself and then insert into them, and the two must not both
 * account for the same newline.  The rectangle's bottom row becomes
 * the last line of the file and gets its terminator.  All of that is
 * one edit to the user: one undo takes it back, one redo restores
 * it. */
void test_rectangle_yank_at_virtual_eof_roundtrip(void) {
	/* A rectangle two wide and two high, copied with C-x r M-w. */
	const char *rect[] = { "ab", "cd" };
	struct buffer *scratch = make_test_buffer_lines(rect, 2);
	scratch->markx = 0;
	scratch->marky = 0;
	scratch->mark_active = 1;
	scratch->cx = 2;
	scratch->cy = 1;
	processKeypress(CMD_COPY_RECT);
	drop_test_buffer(scratch);

	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	char *original = contentOf(buf);

	processKeypress(CMD_YANK);

	char *edited = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING("alpha\nbeta\nab\ncd\n", edited);

	processKeypress(CMD_UNDO);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	processKeypress(CMD_REDO);
	char *redone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(edited, redone);

	free(original);
	free(edited);
	free(undone);
	free(redone);
}


/* Defect: the redo chain had no exit.  After C-/ left more to redo,
 * emil entered a mode where C-_ continued redoing -- but the mode was
 * never cleared when the redo stack emptied, so every later undo
 * keystroke was swallowed as a redo of nothing and undo appeared dead
 * until some unrelated key was pressed.  Found by the fuzzer as
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

/* Each prompt starts with an empty minibuffer, rebuilt outside the
 * mutation layer, so undo history from the previous prompt describes
 * text that is gone.  Replaying it made bulkDelete compute a negative
 * length.  A new prompt must not be able to undo into the last one. */
void test_minibuffer_reset_drops_stale_undo_records(void) {
	makeMinibuffer();
	make_test_buffer("x");

	/* First prompt: type "abc", accept. */
	int first[] = { 'a', 'b', 'c', '\r' };
	scriptKeys(first, 4);
	muteStdout();
	uint8_t *got = editorPrompt("p: ", PROMPT_PLAIN, NULL);
	unmuteStdout();
	clearKeys();
	TEST_ASSERT_EQUAL_STRING("abc", (char *)got);
	free(got);

	/* Second prompt: undo straight away, then type "z" and accept.
	 * The undo has nothing to replay, so the answer is just "z". */
	int second[] = { KEY_CTRL('_'), 'z', '\r' };
	scriptKeys(second, 3);
	muteStdout();
	got = editorPrompt("p: ", PROMPT_PLAIN, NULL);
	unmuteStdout();
	TEST_ASSERT_EQUAL_INT(3, test_key_pos);
	clearKeys();
	TEST_ASSERT_NOT_NULL(got);
	if (got)
		TEST_ASSERT_EQUAL_STRING("z", (char *)got);
	free(got);

	freeMinibuffer();
}

/* ---- Spec 05 P5: where undo and redo leave the cursor ----
 *
 * As in Emacs: undo puts the cursor back where it was just before the
 * command being undone; redo puts it at the start of the change it
 * redoes.  Checked against GNU Emacs 29.3 (emacs -Q) in
 * proof/spec05/emacs/ of the Proof project. */

static void put(struct buffer *buf, int cx, int cy) {
	buf->cx = cx;
	buf->cy = cy;
}

void test_undo_forward_delete_returns_cursor_to_start(void) {
	const char *lines[] = { "abc" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	put(buf, 0, 0);
	processKeypress(CMD_DELETE);
	processKeypress(CMD_DELETE);
	TEST_ASSERT_EQUAL_STRING("c", row_str(buf, 0));
	put(buf, 1, 0); /* move away first */
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
}

void test_undo_backspace_returns_cursor_to_end(void) {
	const char *lines[] = { "abc" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	put(buf, 3, 0);
	processKeypress(CMD_BACKSPACE);
	processKeypress(CMD_BACKSPACE);
	TEST_ASSERT_EQUAL_STRING("a", row_str(buf, 0));
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(3, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
}

void test_undo_kill_line_returns_cursor_to_start(void) {
	const char *lines[] = { "abc", "de" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	put(buf, 1, 0);
	processKeypress(CMD_KILL_LINE);
	TEST_ASSERT_EQUAL_STRING("a", row_str(buf, 0));
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	clearText(&E.kill);
}

void test_undo_kill_region_returns_cursor_where_it_was(void) {
	const char *lines[] = { "abcd" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	/* point before the mark */
	buf->markx = 3;
	buf->marky = 0;
	put(buf, 1, 0);
	processKeypress(CMD_KILL_REGION);
	TEST_ASSERT_EQUAL_STRING("ad", row_str(buf, 0));
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("abcd", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	/* point after the mark */
	buf->markx = 1;
	buf->marky = 0;
	put(buf, 3, 0);
	processKeypress(CMD_KILL_REGION);
	TEST_ASSERT_EQUAL_STRING("ad", row_str(buf, 0));
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("abcd", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(3, buf->cx);
	clearText(&E.kill);
}

void test_redo_typing_puts_cursor_at_start(void) {
	const char *lines[] = { "xy" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	put(buf, 1, 0);
	E.self_insert_key = 'a';
	processKeypress(CMD_SELF_INSERT);
	E.self_insert_key = 'b';
	processKeypress(CMD_SELF_INSERT);
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("xy", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	processKeypress(CMD_REDO);
	TEST_ASSERT_EQUAL_STRING("xaby", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
}

void test_redo_yank_puts_cursor_at_start(void) {
	copyToKillRing("p\nq");
	const char *lines[] = { "xy" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	put(buf, 1, 0);
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("xp", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("qy", row_str(buf, 1));
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	processKeypress(CMD_REDO);
	TEST_ASSERT_EQUAL_STRING("qy", row_str(buf, 1));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
}

/* query-replace's u and U undo the last (all) replacements and offer
 * the match again: the next y must replace that same match. */
static void queryReplaceKeys(const int *keys, int n) {
	makeMinibuffer();
	scriptKeys(keys, n);
	muteStdout();
	processKeypress(CMD_QUERY_REPLACE);
	unmuteStdout();
	clearKeys();
	freeMinibuffer();
}

void test_query_replace_u_offers_the_same_match_again(void) {
	const char *lines[] = { "ab ab" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	put(buf, 0, 0);
	int keys[] = { 'a', 'b', '\r', 'X', '\r', 'y', 'u', 'y', 'q' };
	queryReplaceKeys(keys, 9);
	TEST_ASSERT_EQUAL_STRING("X ab", row_str(buf, 0));
}

void test_query_replace_U_offers_the_first_match_again(void) {
	const char *lines[] = { "ab ab ab" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	put(buf, 0, 0);
	int keys[] = { 'a', 'b', '\r', 'X', '\r', 'y', 'y', 'U', 'y', 'q' };
	queryReplaceKeys(keys, 10);
	TEST_ASSERT_EQUAL_STRING("X ab ab", row_str(buf, 0));
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_undo_delchar_truncated_lead);
	RUN_TEST(test_word_transform_truncated_lead);
	RUN_TEST(test_mutate_refuses_invalid_ranges);
	RUN_TEST(test_mutate_refuses_invalid_insert);
	RUN_TEST(test_mutate_extend_refuses_wrong_row_count);
	RUN_TEST(test_undo_reports_stale_position);
	RUN_TEST(test_undo_reports_changed_text);
	RUN_TEST(test_prompt_tab_undoes);
	RUN_TEST(test_rebuild_without_history_no_record);
	RUN_TEST(test_rebuild_with_history_is_an_edit);
	RUN_TEST(test_redo_reports_changed_text);


	RUN_TEST(test_redo_cleared_after_new_edit);

	RUN_TEST(test_quoted_newline_undo_preserves_following_row);
	RUN_TEST(test_backspace_at_origin_is_a_noop);

	RUN_TEST(test_mutate_replace_readonly);


	RUN_TEST(test_eof_roundtrip_newline);
	RUN_TEST(test_eof_roundtrip_open_line);
	RUN_TEST(test_eof_roundtrip_newline_indent);
	RUN_TEST(test_eof_roundtrip_self_insert);
	RUN_TEST(test_eof_roundtrip_tab);
	RUN_TEST(test_eof_repeated_newline_undo_does_not_accumulate);
	RUN_TEST(test_eof_newline_redo_inserts_one_row_not_two);
	RUN_TEST(test_eof_payload_one_line);
	RUN_TEST(test_eof_payload_two_lines);
	RUN_TEST(test_eof_payload_leading_newline);
	RUN_TEST(test_eof_payload_newlines_only);
	RUN_TEST(test_eof_payload_cjk);
	RUN_TEST(test_eof_payload_trailing_newline_adds_no_extra_row);
	RUN_TEST(test_mark_unmoved_by_newline_at_virtual_eof);
	RUN_TEST(test_mark_unmoved_by_yank_at_virtual_eof);
	RUN_TEST(test_yank_sets_mark_at_start_of_yanked_text);
	RUN_TEST(test_yank_pushes_the_old_mark);
	RUN_TEST(test_newline_and_indent_at_virtual_eof);
	RUN_TEST(test_repeat_count_self_insert_at_virtual_eof);
	RUN_TEST(test_repeat_count_newline_at_virtual_eof);
	RUN_TEST(test_rectangle_yank_at_virtual_eof_roundtrip);
	RUN_TEST(test_redo_chain_releases_undo);
	RUN_TEST(test_minibuffer_reset_drops_stale_undo_records);

	/* Spec 05 P5 */
	RUN_TEST(test_undo_forward_delete_returns_cursor_to_start);
	RUN_TEST(test_undo_backspace_returns_cursor_to_end);
	RUN_TEST(test_undo_kill_line_returns_cursor_to_start);
	RUN_TEST(test_undo_kill_region_returns_cursor_where_it_was);
	RUN_TEST(test_redo_typing_puts_cursor_at_start);
	RUN_TEST(test_redo_yank_puts_cursor_at_start);
	RUN_TEST(test_query_replace_u_offers_the_same_match_again);
	RUN_TEST(test_query_replace_U_offers_the_first_match_again);
	return TEST_END();
}
