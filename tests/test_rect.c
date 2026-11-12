/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_rect.c: Rectangle operation tests.
 *
 * Undo/redo: every rectangle operation (copy, kill, yank,
 * string-replace) can be undone to restore the exact original buffer
 * content, and redo re-applies correctly.
 *
 * UTF-8 safety: rectangle columns are byte offsets, so on interior
 * rows they can land inside a multi-byte sequence.  The operations
 * snap inward to character boundaries; these tests pin the invariant
 * that no rectangle operation ever leaves invalid UTF-8 in a buffer,
 * and that a killed or copied rectangle yanks back as valid UTF-8.
 *
 * The tests set point and mark, issue the command through
 * processKeypress() and read the buffer text.  A rectangle to yank is
 * made the way a user makes one, by copying it out of buffer text;
 * what was killed or copied is checked by yanking it.  The tests do
 * not call the rectangle functions or read the kill buffer, so both
 * can be reshaped without touching this file. */

#include "test.h"
#include "test_harness.h"
#include "keymap.h"
#include "unicode.h"
#include <stdint.h>
#include <string.h>
#include "util.h"

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

/* Snapshot all row content so we can compare after undo. */
static char **snapshot_buffer(struct buffer *buf, int *nrows) {
	*nrows = buf->numrows;
	char **snap = calloc(buf->numrows, sizeof(char *));
	for (int i = 0; i < buf->numrows; i++)
		snap[i] = xstrdup((char *)buf->row[i].chars);
	return snap;
}

static void assert_buffer_matches(struct buffer *buf, char **snap, int nrows,
				  const char *label) {
	if (buf->numrows != nrows) {
		printf("  FAIL (%s): row count %d vs expected %d\n", label,
		       buf->numrows, nrows);
		_current_test_failed = 1;
		return;
	}
	for (int i = 0; i < nrows; i++) {
		if (strcmp((char *)buf->row[i].chars, snap[i]) != 0) {
			printf("  FAIL (%s): row %d: \"%s\" vs expected \"%s\"\n",
			       label, i, (char *)buf->row[i].chars, snap[i]);
			_current_test_failed = 1;
		}
	}
}

static void free_snapshot(char **snap, int nrows) {
	for (int i = 0; i < nrows; i++)
		free(snap[i]);
	free(snap);
}

/* Set mark at (mx, my) and activate it. */
static void set_mark(struct buffer *buf, int mx, int my) {
	buf->markx = mx;
	buf->marky = my;
	buf->mark_active = 1;
}

/* Put an h-row rectangle on the kill ring the way a user would: copy
 * it (C-x r M-w) out of a buffer holding exactly those rows.  The
 * rows must be equally long.  The scratch buffer is gone on return,
 * so call this before making the buffer under test. */
static void copy_rect_of(const char **rows, int h) {
	struct buffer *src = make_test_buffer_lines(rows, h);
	src->cx = 0;
	src->cy = 0;
	set_mark(src, (int)strlen(rows[0]), h - 1);
	processKeypress(CMD_COPY_RECT);
	destroyBuffer(src);
	E.buf = NULL;
	E.headbuf = NULL;
	E.windows[0]->buf = NULL;
}

/* Run a command that reads from the minibuffer, with `typed` and RET
 * waiting for it.  Returns how many of those keys were read, so a
 * test can tell a command that prompted from one that refused first. */
static int run_typing(int cmd, const char *typed) {
	int keys[64];
	int n = 0;
	while (typed[n] && n < 63) {
		keys[n] = (unsigned char)typed[n];
		n++;
	}
	keys[n++] = '\r';

	makeMinibuffer();
	scriptKeys(keys, n);
	muteStdout();
	processKeypress(cmd);
	unmuteStdout();
	int used = test_key_pos;
	clearKeys();
	freeMinibuffer();
	return used;
}

/* ----------------------------------------------------------------
 * Kill rectangle (C-x r k)
 * ---------------------------------------------------------------- */

void test_kill_rect_multi_row_undo(void) {
	const char *lines[] = { "ABCDE", "FGHIJ", "KLMNO" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Rectangle: columns 1-3, rows 0-2 */
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 2);
	processKeypress(CMD_KILL_RECT);

	TEST_ASSERT_EQUAL_STRING("AE", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("FJ", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("KO", row_str(buf, 2));

	processKeypress(CMD_UNDO);
	assert_buffer_matches(buf, snap, snap_n, "kill_rect_multi_undo");

	free_snapshot(snap, snap_n);
}

void test_kill_rect_multi_row_redo(void) {
	const char *lines[] = { "ABCDE", "FGHIJ", "KLMNO" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 2);
	processKeypress(CMD_KILL_RECT);

	/* Capture post-kill state */
	int kill_n;
	char **kill_snap = snapshot_buffer(buf, &kill_n);

	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("ABCDE", row_str(buf, 0));
	processKeypress(CMD_REDO);

	assert_buffer_matches(buf, kill_snap, kill_n, "kill_rect_redo");

	free_snapshot(kill_snap, kill_n);
}

void test_kill_rect_short_rows_undo(void) {
	/* Rows shorter than the rectangle right edge */
	const char *lines[] = { "ABCDEFGH", "AB", "ABCDEFGH" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Rectangle: columns 2-6, rows 0-2 */
	buf->cx = 2;
	buf->cy = 0;
	set_mark(buf, 6, 2);
	processKeypress(CMD_KILL_RECT);

	/* The short row ends before the rectangle starts: it loses
	 * nothing and gains no padding. */
	TEST_ASSERT_EQUAL_STRING("ABGH", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("AB", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("ABGH", row_str(buf, 2));

	processKeypress(CMD_UNDO);
	assert_buffer_matches(buf, snap, snap_n, "kill_rect_short_rows_undo");

	free_snapshot(snap, snap_n);
}

void test_kill_rect_swapped_columns_undo(void) {
	/* Point is on the first row but at the right-hand column, mark
	 * on the last row at the left-hand column: the rectangle is
	 * still columns 1-3 of both rows. */
	const char *lines[] = { "ABCDE", "FGHIJ" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 4;
	buf->cy = 0;
	set_mark(buf, 1, 1);
	processKeypress(CMD_KILL_RECT);

	TEST_ASSERT_EQUAL_STRING("AE", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("FJ", row_str(buf, 1));

	processKeypress(CMD_UNDO);
	assert_buffer_matches(buf, snap, snap_n, "kill_rect_swapped_cols_undo");

	free_snapshot(snap, snap_n);
}

/* ----------------------------------------------------------------
 * Copy rectangle (C-x r M-w): buffer untouched, rectangle yankable
 * ---------------------------------------------------------------- */

void test_copy_rect_preserves_buffer(void) {
	const char *lines[] = { "ABCDE", "FGHIJ", "KLMNO" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 2);
	processKeypress(CMD_COPY_RECT);

	/* Buffer must be unchanged, and not marked as modified */
	assert_buffer_matches(buf, snap, snap_n, "copy_rect_preserves");
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	/* What was copied is the 3x3 block "BCD" / "GHI" / "LMN":
	 * yanked at the top-left corner it lands in front of each row. */
	buf->cx = 0;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);
	TEST_ASSERT_EQUAL_STRING("BCDABCDE", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("GHIFGHIJ", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("LMNKLMNO", row_str(buf, 2));
	TEST_ASSERT_EQUAL_INT(4, buf->numrows);

	free_snapshot(snap, snap_n);
}

/* ----------------------------------------------------------------
 * Yank rectangle (C-x r y)
 * ---------------------------------------------------------------- */

void test_yank_rect_basic_undo(void) {
	/* A 2-wide, 3-tall rectangle on the kill ring */
	const char *rect[] = { "XX", "YY", "ZZ" };
	copy_rect_of(rect, 3);

	const char *lines[] = { "AAAA", "BBBB", "CCCC" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 1;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);

	/* Each rectangle row is inserted at column 1 of its buffer row */
	TEST_ASSERT_EQUAL_STRING("AXXAAA", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("BYYBBB", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("CZZCCC", row_str(buf, 2));

	processKeypress(CMD_UNDO);
	assert_buffer_matches(buf, snap, snap_n, "yank_rect_basic_undo");

	free_snapshot(snap, snap_n);
}

void test_yank_rect_redo(void) {
	const char *rect[] = { "XX", "YY", "ZZ" };
	copy_rect_of(rect, 3);

	const char *lines[] = { "AAAA", "BBBB", "CCCC" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	buf->cx = 1;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);

	int yank_n;
	char **yank_snap = snapshot_buffer(buf, &yank_n);

	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("AAAA", row_str(buf, 0));
	processKeypress(CMD_REDO);
	assert_buffer_matches(buf, yank_snap, yank_n, "yank_rect_redo");

	free_snapshot(yank_snap, yank_n);
}

void test_yank_rect_extra_lines_undo(void) {
	/* Yanking a 4-row rectangle into a 2-row buffer requires
	 * adding extra lines, and one undo must take all of it back. */
	const char *rect[] = { "XX", "YY", "ZZ", "WW" };
	copy_rect_of(rect, 4);

	const char *lines[] = { "AA", "BB" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 0;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);

	/* Four rows of rectangle, plus the empty row that terminates
	 * the last of them: the rectangle's bottom row is now the last
	 * line of the file and needs its newline like any other. */
	TEST_ASSERT_EQUAL_INT(5, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("XXAA", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("YYBB", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("ZZ", row_str(buf, 2));
	TEST_ASSERT_EQUAL_STRING("WW", row_str(buf, 3));
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 4));

	processKeypress(CMD_UNDO);

	/* Undo should fully restore original buffer */
	assert_buffer_matches(buf, snap, snap_n, "yank_rect_extra_lines_undo");

	free_snapshot(snap, snap_n);
}

void test_yank_rect_into_short_rows_undo(void) {
	/* Rows shorter than the insertion column — tests space padding.
	 * Undo must remove both the inserted rectangle data AND the
	 * space padding added to reach the insertion column. */
	const char *rect[] = { "XX", "YY", "ZZ" };
	copy_rect_of(rect, 3);

	const char *lines[] = { "AAAAAA", "B", "C" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Insert at column 5 — well past the end of the 1-char rows
	 * below the one point is on */
	buf->cx = 5;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);

	/* The short rows are padded with spaces out to column 5 */
	TEST_ASSERT_EQUAL_STRING("AAAAAXXA", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("B    YY", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("C    ZZ", row_str(buf, 2));

	processKeypress(CMD_UNDO);

	/* Undo should fully restore original buffer */
	assert_buffer_matches(buf, snap, snap_n, "yank_rect_short_rows_undo");

	free_snapshot(snap, snap_n);
}

/* ----------------------------------------------------------------
 * Linear kill (C-w) undo — baseline for comparison
 * ---------------------------------------------------------------- */

void test_kill_region_undo(void) {
	const char *lines[] = { "Hello", "World" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* From (2,0) to (3,1): "llo\nWor" */
	buf->cx = 2;
	buf->cy = 0;
	set_mark(buf, 3, 1);
	processKeypress(CMD_KILL_REGION);
	TEST_ASSERT_EQUAL_STRING("Held", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);

	processKeypress(CMD_UNDO);
	assert_buffer_matches(buf, snap, snap_n, "kill_region_undo");

	free_snapshot(snap, snap_n);
}

/* ----------------------------------------------------------------
 * Stress: undo through multiple rectangle operations
 * ---------------------------------------------------------------- */

void test_multiple_rect_ops_undo_all(void) {
	const char *lines[] = { "ABCD", "EFGH", "IJKL" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* First: kill rectangle cols 0-2 */
	buf->cx = 0;
	buf->cy = 0;
	set_mark(buf, 2, 2);
	processKeypress(CMD_KILL_RECT);
	TEST_ASSERT_EQUAL_STRING("CD", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("GH", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("KL", row_str(buf, 2));

	/* Second: yank it back, but at the other end of the rows */
	buf->cx = 2;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);
	TEST_ASSERT_EQUAL_STRING("CDAB", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("GHEF", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("KLIJ", row_str(buf, 2));

	/* Undo yank */
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("CD", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("GH", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("KL", row_str(buf, 2));
	/* Undo kill */
	processKeypress(CMD_UNDO);

	assert_buffer_matches(buf, snap, snap_n, "multiple_rect_ops_undo");

	free_snapshot(snap, snap_n);
}

/* ----------------------------------------------------------------
 * Edge case: zero-width rectangle (topx == botx)
 * ---------------------------------------------------------------- */

void test_kill_rect_zero_width(void) {
	const char *lines[] = { "ABCDE" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 2;
	buf->cy = 0;
	set_mark(buf, 2, 0);
	/* Mark and point are the same place, so there is no rectangle
	 * and the buffer stays intact */
	processKeypress(CMD_KILL_RECT);

	assert_buffer_matches(buf, snap, snap_n, "kill_rect_zero_width");
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	free_snapshot(snap, snap_n);
}

/* ----------------------------------------------------------------
 * Edge case: yank rectangle into an empty buffer
 * ---------------------------------------------------------------- */

void test_yank_rect_into_empty_buffer(void) {
	/* Regression: kill a rectangle in one buffer, then yank into an
	 * empty buffer (e.g. a freshly opened empty file).
	 */
	const char *lines[] = { "ABCDE", "FGHIJ" };
	struct buffer *src = make_test_buffer_lines(lines, 2);

	/* Kill columns 1-3, rows 0-1 => "BCD"/"GHI" */
	src->cx = 1;
	src->cy = 0;
	set_mark(src, 4, 1);
	processKeypress(CMD_KILL_RECT);

	/* Fresh empty buffer: one empty row.
	 * Wiring it into E orphans src, so destroy src explicitly. */
	struct buffer *dst = make_test_buffer("");
	destroyBuffer(src);
	TEST_ASSERT_EQUAL_INT(1, dst->numrows);

	processKeypress(CMD_YANK_RECT);

	/* Two rows of rectangle plus the terminating empty row. */
	TEST_ASSERT_EQUAL_INT(3, dst->numrows);
	TEST_ASSERT_EQUAL_STRING("BCD", row_str(dst, 0));
	TEST_ASSERT_EQUAL_STRING("GHI", row_str(dst, 1));
	TEST_ASSERT_EQUAL_STRING("", row_str(dst, 2));
	TEST_ASSERT_EQUAL_INT(0, dst->cx);
	TEST_ASSERT_EQUAL_INT(0, dst->cy);

	/* Undo the whole chain: back to the single empty row. */
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_INT(1, dst->numrows);
	TEST_ASSERT_EQUAL_STRING("", row_str(dst, 0));

	/* Redo restores the yanked rectangle. */
	processKeypress(CMD_REDO);
	TEST_ASSERT_EQUAL_INT(3, dst->numrows);
	TEST_ASSERT_EQUAL_STRING("BCD", row_str(dst, 0));
	TEST_ASSERT_EQUAL_STRING("GHI", row_str(dst, 1));
	TEST_ASSERT_EQUAL_STRING("", row_str(dst, 2));
}

/* ----------------------------------------------------------------
 * Read-only refusal. All refuse before prompting,
 * ---------------------------------------------------------------- */

void test_kill_rectangle_readonly(void) {
	const char *lines[] = { "ABCDE", "FGHIJ", "KLMNO" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->read_only = 1;
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 2);
	processKeypress(CMD_KILL_RECT);
	TEST_ASSERT_EQUAL_STRING("ABCDE", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("FGHIJ", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("KLMNO", row_str(buf, 2));
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
	buf->read_only = 0;
}

void test_string_rectangle_readonly(void) {
	const char *lines[] = { "ABCDE", "FGHIJ" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->read_only = 1;
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 3, 1);

	/* C-x r t with "ZZ" RET waiting: refused before the prompt, so
	 * none of it is read. */
	int used = run_typing(CMD_STRING_RECT, "ZZ");
	TEST_ASSERT_EQUAL_INT(0, used);
	TEST_ASSERT_EQUAL_STRING("ABCDE", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("FGHIJ", row_str(buf, 1));
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
	buf->read_only = 0;
}

void test_replace_regex_readonly(void) {
	const char *lines[] = { "alpha beta", "gamma beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->read_only = 1;
	buf->cx = 0;
	buf->cy = 0;
	set_mark(buf, 10, 1);

	/* M-x replace-regexp RET, with a pattern and a replacement
	 * waiting behind it.  The command name and its RET are read;
	 * the command then refuses before asking for the pattern, so
	 * nothing after that is. */
	const char *name = "replace-regexp";
	int keys[64];
	int n = 0;
	for (const char *c = name; *c; c++)
		keys[n++] = (unsigned char)*c;
	keys[n++] = '\r';
	int name_keys = n;
	for (const char *c = "beta"; *c; c++)
		keys[n++] = (unsigned char)*c;
	keys[n++] = '\r';
	keys[n++] = 'X';
	keys[n++] = '\r';

	setupCommands();
	makeMinibuffer();
	scriptKeys(keys, n);
	muteStdout();
	processKeypress(CMD_EXEC_CMD);
	unmuteStdout();
	int used = test_key_pos;
	clearKeys();
	freeMinibuffer();

	TEST_ASSERT_EQUAL_INT(name_keys, used);
	TEST_ASSERT_EQUAL_STRING("alpha beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("gamma beta", row_str(buf, 1));
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
	buf->read_only = 0;
}

/* ----------------------------------------------------------------
 * UTF-8 safety
 *
 * "日本語" is E6 97 A5 / E6 9C AC / E8 AA 9E — three 3-byte
 * characters.  Byte columns [1,3) land inside 日 on that row while
 * being perfectly aligned on the ASCII anchor rows, which is the
 * corruption case: a well-anchored rectangle slicing through a
 * multi-byte character on an interior row.
 * ---------------------------------------------------------------- */

#define NIHONGO "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"

static void assert_all_rows_valid(struct buffer *buf, const char *label) {
	for (int i = 0; i < buf->numrows; i++) {
		if (!utf8_validate(buf->row[i].chars, buf->row[i].size)) {
			printf("  FAIL (%s): row %d contains invalid UTF-8\n",
			       label, i);
			_current_test_failed = 1;
		}
	}
}

void test_kill_rect_utf8_interior_row(void) {
	const char *lines[] = { "abcd", NIHONGO, "abcd" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Cols [1,3): splits 日 and 本 on row 1.  Both partially
	 * covered characters must stay whole in the buffer and be
	 * excluded from the kill. */
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 3, 2);
	processKeypress(CMD_KILL_RECT);

	assert_all_rows_valid(buf, "kill_rect_utf8");
	TEST_ASSERT_EQUAL_STRING("ad", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING(NIHONGO, row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("ad", row_str(buf, 2));

	/* Cursor and mark must land on character boundaries */
	TEST_ASSERT(buf->cx == 0 || !utf8_isCont(buf->row[buf->cy].chars[buf->cx]));

	processKeypress(CMD_UNDO);
	assert_buffer_matches(buf, snap, snap_n, "kill_rect_utf8_undo");

	/* What was killed is "bc" / two spaces / "bc": the middle row
	 * gave up no bytes, so its slice is padding.  Yanked at the
	 * top-left corner it shows as such, and every row is still
	 * valid UTF-8. */
	buf->cx = 0;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);
	assert_all_rows_valid(buf, "kill_rect_utf8_yank");
	TEST_ASSERT_EQUAL_STRING("bcabcd", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("  " NIHONGO, row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("bcabcd", row_str(buf, 2));

	free_snapshot(snap, snap_n);
}

void test_kill_rect_utf8_whole_char_covered(void) {
	/* Cols [1,4) fully cover 日 on the interior row: it must be
	 * extracted, not skipped.
	 *
	 * The parentheses are not decoration.  Concatenating two string
	 * literals inside an array initialiser is exactly what a missing
	 * comma looks like, so clang warns (-Wstring-concatenation, on
	 * under -Wextra since at least clang 18) and `hal` builds tests
	 * with -Werror.  Parenthesising is clang's own suggested way to
	 * say the concatenation is deliberate.  gcc has no such warning,
	 * and `hal` runs CC=cc, which is why this went unnoticed. */
	const char *lines[] = { "aaaaa", ("a" NIHONGO), "aaaaa" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 2);
	processKeypress(CMD_KILL_RECT);

	assert_all_rows_valid(buf, "kill_rect_utf8_whole");
	TEST_ASSERT_EQUAL_STRING("aa", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("a\xE6\x9C\xAC\xE8\xAA\x9E",
				 row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("aa", row_str(buf, 2));

	/* The killed rows are "aaa", "日", "aaa": yanked into a buffer
	 * of dots, the character arrives whole. */
	const char *lines2[] = { "..", "..", ".." };
	struct buffer *buf2 = make_test_buffer_lines(lines2, 3);
	buf2->cx = 0;
	buf2->cy = 0;
	processKeypress(CMD_YANK_RECT);

	assert_all_rows_valid(buf2, "kill_rect_utf8_whole_yank");
	TEST_ASSERT_EQUAL_STRING("aaa..", row_str(buf2, 0));
	TEST_ASSERT_EQUAL_STRING("\xE6\x97\xA5"
				 "..",
				 row_str(buf2, 1));
	TEST_ASSERT_EQUAL_STRING("aaa..", row_str(buf2, 2));

	/* buf was displaced from E.headbuf by make_test_buffer_lines;
	 * destroy it here so the harness cleanup sees no leak. */
	destroyBuffer(buf);
}

void test_copy_rect_utf8_interior_row(void) {
	const char *lines[] = { "abcd", NIHONGO, "abcd" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 3, 2);
	processKeypress(CMD_COPY_RECT);

	/* Copy must not touch the buffer. */
	assert_buffer_matches(buf, snap, snap_n, "copy_rect_utf8");

	/* What it copied must be valid UTF-8: the mid-character slice
	 * on row 1 is space padding.  Yanked at the end of the first
	 * row, column 4, which is inside 本 on the row below: there
	 * the padding goes in after 本, not through it. */
	buf->cx = 4;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);
	assert_all_rows_valid(buf, "copy_rect_utf8_yank");
	TEST_ASSERT_EQUAL_STRING("abcdbc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("\xE6\x97\xA5\xE6\x9C\xAC"
				 "  \xE8\xAA\x9E",
				 row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("abcdbc", row_str(buf, 2));

	free_snapshot(snap, snap_n);
}

void test_yank_rect_utf8_into_ascii(void) {
	/* A rectangle copied across a multibyte interior row must
	 * yank cleanly into a pure-ASCII buffer. */
	const char *lines[] = { "abcd", NIHONGO, "abcd" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 3, 2);
	processKeypress(CMD_COPY_RECT);

	const char *lines2[] = { "....", "....", "...." };
	struct buffer *buf2 = make_test_buffer_lines(lines2, 3);
	E.buf = buf2;
	buf2->cx = 0;
	buf2->cy = 0;
	processKeypress(CMD_YANK_RECT);

	assert_all_rows_valid(buf2, "yank_rect_utf8");
	TEST_ASSERT_EQUAL_STRING("bc....", row_str(buf2, 0));
	TEST_ASSERT_EQUAL_STRING("  ....", row_str(buf2, 1));
	TEST_ASSERT_EQUAL_STRING("bc....", row_str(buf2, 2));

	/* buf was displaced from E.headbuf by make_test_buffer_lines;
	 * destroy it here so the harness cleanup sees no leak. */
	destroyBuffer(buf);
}

void test_yank_rect_utf8_destination_midchar(void) {
	/* Yanking at a byte offset that falls mid-character on a
	 * destination row must not split that character: the
	 * insertion point snaps forward to the next boundary. */
	const char *lines[] = { "abc", "abc" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 2, 1);
	processKeypress(CMD_COPY_RECT); /* 1x2 rect: "b","b" */

	const char *lines2[] = { "axxx", NIHONGO };
	struct buffer *buf2 = make_test_buffer_lines(lines2, 2);
	E.buf = buf2;
	buf2->cx = 1; /* boundary in row 0, mid-日 in row 1 */
	buf2->cy = 0;
	processKeypress(CMD_YANK_RECT);

	assert_all_rows_valid(buf2, "yank_rect_utf8_dest");
	TEST_ASSERT_EQUAL_STRING("abxxx", row_str(buf2, 0));
	TEST_ASSERT_EQUAL_STRING("\xE6\x97\xA5"
				 "b\xE6\x9C\xAC\xE8\xAA\x9E",
				 row_str(buf2, 1));

	destroyBuffer(buf);
}

void test_string_rect_utf8_interior_row(void) {
	const char *lines[] = { "abcd", NIHONGO, "abcd" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* C-x r t, then "XY" RET at its prompt */
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 3, 2);
	int used = run_typing(CMD_STRING_RECT, "XY");
	TEST_ASSERT_EQUAL_INT(3, used);

	assert_all_rows_valid(buf, "string_rect_utf8");
	TEST_ASSERT_EQUAL_STRING("aXYd", row_str(buf, 0));
	/* Cols [1,3) lie inside 日: nothing is removed on that row,
	 * the string lands between the whole characters. */
	TEST_ASSERT_EQUAL_STRING("\xE6\x97\xA5"
				 "XY\xE6\x9C\xAC\xE8\xAA\x9E",
				 row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("aXYd", row_str(buf, 2));

	processKeypress(CMD_UNDO);
	assert_buffer_matches(buf, snap, snap_n, "string_rect_utf8_undo");
	free_snapshot(snap, snap_n);
}

/* ----------------------------------------------------------------
 * Runner
 * ---------------------------------------------------------------- */

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

int main(void) {
	TEST_BEGIN();

	/* Kill rectangle */
	RUN_TEST(test_kill_rect_multi_row_undo);
	RUN_TEST(test_kill_rect_multi_row_redo);
	RUN_TEST(test_kill_rect_short_rows_undo);
	RUN_TEST(test_kill_rect_swapped_columns_undo);

	/* Read-only refusal */
	RUN_TEST(test_kill_rectangle_readonly);
	RUN_TEST(test_string_rectangle_readonly);
	RUN_TEST(test_replace_regex_readonly);

	/* Copy rectangle */
	RUN_TEST(test_copy_rect_preserves_buffer);

	/* Yank rectangle */
	RUN_TEST(test_yank_rect_basic_undo);
	RUN_TEST(test_yank_rect_redo);
	RUN_TEST(test_yank_rect_extra_lines_undo);
	RUN_TEST(test_yank_rect_into_short_rows_undo);

	/* Linear kill (baseline) */
	RUN_TEST(test_kill_region_undo);

	/* Multiple operations */
	RUN_TEST(test_multiple_rect_ops_undo_all);

	/* Edge cases */
	RUN_TEST(test_kill_rect_zero_width);
	RUN_TEST(test_yank_rect_into_empty_buffer);

	/* UTF-8 safety */
	RUN_TEST(test_kill_rect_utf8_interior_row);
	RUN_TEST(test_kill_rect_utf8_whole_char_covered);
	RUN_TEST(test_copy_rect_utf8_interior_row);
	RUN_TEST(test_yank_rect_utf8_into_ascii);
	RUN_TEST(test_yank_rect_utf8_destination_midchar);
	RUN_TEST(test_string_rect_utf8_interior_row);

	return TEST_END();
}
