/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_adjust.c: Tests for adjustPoint and adjustAllPoints.
 *
 * adjustPoint is pure logic: given a tracked point and a mutation range,
 * compute the new point position.  These tests cover every branch for
 * both insertions and deletions, plus edge cases at exact boundaries. */

#include "test.h"
#include "test_harness.h"
#include "adjust.h"
#include "edit.h"
#include "region.h"
#include "unicode.h"
#include "undo.h"
#include <string.h>

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- Deletion: point before region ---- */

void test_del_point_before_same_line(void) {
	int px = 2, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_del_point_before_earlier_line(void) {
	int px = 5, py = 1;
	int ret = adjustPoint(&px, &py, 3, 5, 8, 7, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(1, py);
}

void test_del_point_at_start(void) {
	/* Point at exact start of deletion — "before" branch (<=) */
	int px = 5, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

/* ---- Deletion: point inside region ---- */

void test_del_point_inside_single_line(void) {
	int px = 7, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(1, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_del_point_inside_multiline(void) {
	/* Delete from (5,3) to (8,7), point at (2,5) is inside */
	int px = 2, py = 5;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 7, 1);
	TEST_ASSERT_EQUAL_INT(1, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_del_point_at_end(void) {
	/* Point at exact end of deletion — inside (<=) */
	int px = 10, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(1, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

/* ---- Deletion: point after region, same end line ---- */

void test_del_point_after_on_end_line(void) {
	/* Delete (5,3) to (10,3), point at (15,3) → should shift left */
	int px = 15, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(10, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_del_point_after_on_end_line_multiline(void) {
	/* Delete (5,3) to (8,7), point at (20,7) → col adjusts to start line */
	int px = 20, py = 7;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 7, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(17, px); /* 5 + (20 - 8) */
	TEST_ASSERT_EQUAL_INT(3, py);
}

/* ---- Deletion: point after region, later line ---- */

void test_del_point_after_later_line(void) {
	/* Delete (5,3) to (8,7), point at (2,10) → row shifts up by 4 */
	int px = 2, py = 10;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 7, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(6, py); /* 10 - (7-3) */
}

/* ---- Insertion: point before region ---- */

void test_ins_point_before_same_line(void) {
	int px = 2, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_ins_point_before_earlier_line(void) {
	int px = 5, py = 1;
	int ret = adjustPoint(&px, &py, 3, 5, 8, 7, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(1, py);
}

/* ---- Insertion: point at insertion point (same-line) ---- */

void test_ins_point_at_start_same_line(void) {
	/* Insert 5 chars at (5,3), ending at (10,3), point at (5,3):
	 * like an Emacs marker, the point stays before the new text. */
	int px = 5, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_ins_point_at_start_multiline(void) {
	/* Insert at (5,3) ending at (8,5), point at (5,3): stays. */
	int px = 5, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 5, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_ins_point_after_on_same_line(void) {
	/* Insert at (5,3) ending at (10,3), point at (8,3) → shifts right */
	int px = 8, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(13, px); /* 8 + (10-5) */
	TEST_ASSERT_EQUAL_INT(3, py);
}

/* ---- Insertion: point on insertion line, multi-line insert ---- */

void test_ins_multiline_point_on_start_line(void) {
	/* Insert at (5,3) ending at (8,5), point at (10,3) */
	int px = 10, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 5, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(13, px); /* 8 + (10 - 5) */
	TEST_ASSERT_EQUAL_INT(5, py);  /* 3 + (5-3) */
}

/* ---- Insertion: point after insertion line ---- */

void test_ins_point_after_later_line(void) {
	/* Insert at (5,3) ending at (8,5), point at (2,10) → row shifts down */
	int px = 2, py = 10;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 5, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(12, py); /* 10 + (5-3) */
}

void test_ins_point_after_single_line(void) {
	/* Single-line insert, point on later line — no column change */
	int px = 2, py = 10;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(10, py); /* no line delta */
}

/* ----------------------------------------------------------------
 * Mark ring
 *
 * Ring entries are byte offsets, exactly like the live mark, so
 * adjustAllPoints() must keep them current across mutations.  When it
 * did not, popMark() restored positions that no longer existed: it
 * jumped to the wrong place (pure ASCII, no Unicode needed) and could
 * put the mark inside a multi-byte character, so typing there left
 * invalid UTF-8 in the buffer -- which no load path will reopen.
 *
 * These drive the real edit functions rather than calling adjustPoint
 * directly, so they pin the whole path: push -> mutate -> pop.
 * ---------------------------------------------------------------- */

/* Push (cx,cy) onto the ring the way a real C-SPC does. */
static void push_mark_at(struct buffer *buf, int cx, int cy) {
	buf->cx = cx;
	buf->cy = cy;
	setMark();
}

void test_markring_adjusted_on_delete(void) {
	const char *lines[] = { "hello world" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);

	push_mark_at(buf, 6, 0); /* mark at "world" */
	push_mark_at(buf, 0, 0); /* pushes (6,0) onto the ring */
	TEST_ASSERT_EQUAL_INT(6, buf->mark_ring[0].cx);

	/* Delete one char at the start: everything after shifts left */
	buf->cx = 0;
	buf->cy = 0;
	delChar(1);

	/* The ring entry must have tracked the edit, like the live mark */
	TEST_ASSERT_EQUAL_INT(5, buf->mark_ring[0].cx);
	TEST_ASSERT_EQUAL_INT(0, buf->mark_ring[0].cy);
}

void test_markring_adjusted_on_insert(void) {
	const char *lines[] = { "hello" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);

	push_mark_at(buf, 5, 0);
	push_mark_at(buf, 0, 0);
	TEST_ASSERT_EQUAL_INT(5, buf->mark_ring[0].cx);

	/* selfInsert() is the undoable typing path, and the mutation
	 * layer inside it drives adjustAllPoints.  Mirror the real call
	 * site (keymap.c) rather than calling the raw insertChar
	 * primitive bare. */
	buf->cx = 0;
	buf->cy = 0;
	selfInsert(buf, 'X', 1);

	TEST_ASSERT_EQUAL_INT(6, buf->mark_ring[0].cx);
}

void test_markring_pop_jumps_to_right_line(void) {
	/* Bug 2b: ASCII-only wrong jump.  Mark a line, kill lines
	 * above it, pop back -- must still land on the same text. */
	const char *lines[] = { "AAA", "BBB", "CCC", "DDD",
				"TARGET", "FFF", "GGG" };
	struct buffer *buf = make_test_buffer_lines(lines, 7);

	push_mark_at(buf, 0, 4); /* on TARGET */
	push_mark_at(buf, 0, 0); /* ring gets (0,4) */

	/* Kill the four lines above TARGET */
	buf->cx = 0;
	buf->cy = 0;
	for (int i = 0; i < 4; i++)
		killLine(1);

	TEST_ASSERT_EQUAL_STRING("TARGET", (char *)buf->row[2].chars);

	popMark();

	/* popMark moves point to the old live mark and rotates the
	 * ring entry into the mark; the entry must now name TARGET. */
	TEST_ASSERT_EQUAL_INT(2, buf->marky);
	TEST_ASSERT_EQUAL_STRING("TARGET", (char *)buf->row[buf->marky].chars);
}

void test_markring_pop_stays_on_char_boundary(void) {
	/* Bug 2a: stale entry landing mid-character.  "xcafé" is
	 * 78 63 61 66 C3 A9; byte 4 is the start of 'é'.  Deleting
	 * the leading 'x' shifts it to 3, so an unadjusted entry of 4
	 * points into the middle of the character. */
	const char *lines[] = { "xcaf\xC3\xA9" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);

	push_mark_at(buf, 4, 0); /* start of 'é' */
	push_mark_at(buf, 0, 0); /* ring gets (4,0) */

	buf->cx = 0;
	buf->cy = 0;
	delChar(1); /* delete 'x' */
	TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9", (char *)buf->row[0].chars);

	popMark();

	/* The mark must have followed the character, not the offset */
	TEST_ASSERT_EQUAL_INT(3, buf->markx);
	TEST_ASSERT_EQUAL_INT(0, buf->marky);
	TEST_ASSERT(buf->markx == 0 ||
		    !utf8_isCont(buf->row[buf->marky].chars[buf->markx]));

	/* Typing at the mark (as C-x C-x then self-insert does) must
	 * leave the row valid: this is the corruption endpoint. */
	buf->cx = buf->markx;
	buf->cy = buf->marky;
	selfInsert(buf, 'Z', 1);
	TEST_ASSERT(utf8_validate(buf->row[0].chars, buf->row[0].size));
	TEST_ASSERT_EQUAL_STRING("cafZ\xC3\xA9", (char *)buf->row[0].chars);
}

void test_markring_pop_clamps_out_of_range_entry(void) {
	/* Backstop: an entry that somehow outlives its text must be
	 * clamped rather than restored out of range. */
	const char *lines[] = { "AAA", "BBB", "CCC" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	push_mark_at(buf, 0, 2);
	push_mark_at(buf, 0, 0);

	/* Forge a stale entry past the end of the buffer */
	buf->mark_ring[0].cx = 99;
	buf->mark_ring[0].cy = 99;

	popMark();

	TEST_ASSERT(buf->marky >= 0 && buf->marky < buf->numrows);
	TEST_ASSERT(buf->markx >= 0 &&
		    buf->markx <= buf->row[buf->marky].size);
}

void test_markring_multiple_entries_all_adjusted(void) {
	/* Every valid entry must be adjusted, not just the newest. */
	const char *lines[] = { "AAA", "BBB", "CCC", "DDD", "EEE" };
	struct buffer *buf = make_test_buffer_lines(lines, 5);

	/* The first setMark has no prior mark to push, so four calls
	 * are needed to leave three entries on the ring. */
	push_mark_at(buf, 0, 1);
	push_mark_at(buf, 0, 2);
	push_mark_at(buf, 0, 3);
	push_mark_at(buf, 0, 4);
	TEST_ASSERT_EQUAL_INT(3, buf->mark_ring_len);

	/* Delete the first line (kill to EOL, then the newline):
	 * every entry below shifts up by one */
	buf->cx = 0;
	buf->cy = 0;
	killLine(1);
	killLine(1);
	TEST_ASSERT_EQUAL_STRING("BBB", (char *)buf->row[0].chars);

	TEST_ASSERT_EQUAL_INT(0, buf->mark_ring[0].cy); /* was 1 */
	TEST_ASSERT_EQUAL_INT(1, buf->mark_ring[1].cy); /* was 2 */
	TEST_ASSERT_EQUAL_INT(2, buf->mark_ring[2].cy); /* was 3 */
}

/* ----------------------------------------------------------------
 * Viewport top (win->rowoff)
 *
 * rowoff is a tracked position that adjustAllPoints did not know
 * about, so an edit above a window's viewport top slid the text under
 * that window while its status bar line number -- taken from the
 * adjusted cy -- stayed correct, and the two disagreed.
 *
 * rowoff is a row index, not a position, so it is adjusted at row
 * granularity for EVERY window on the buffer, including the focused
 * one: the command moves the focused window's cursor, but nothing
 * moves its viewport.
 * ---------------------------------------------------------------- */

/* Two windows on one buffer; the second is scrolled down and
 * unfocused.  Returns the buffer, with rows named L00..L19. */
static struct buffer *two_windows_scrolled(int rowoff) {
	E.windows = xrealloc(E.windows, 2 * sizeof(struct window *));
	E.windows[1] = xcalloc(1, sizeof(struct window));
	E.nwindows = 2;
	E.windows[0]->focused = 1;
	E.windows[1]->focused = 0;
	E.windows[0]->height = 10;
	E.windows[1]->height = 10;

	static char names[20][16];
	const char *lines[20];
	for (int i = 0; i < 20; i++) {
		snprintf(names[i], sizeof(names[i]), "L%02d", i);
		lines[i] = names[i];
	}
	struct buffer *buf = make_test_buffer_lines(lines, 20);

	E.windows[1]->buf = buf;
	E.windows[1]->rowoff = rowoff;
	E.windows[1]->cy = rowoff + 2;
	E.windows[1]->cx = 0;
	return buf;
}

void test_rowoff_tracks_text_across_delete_above(void) {
	struct buffer *buf = two_windows_scrolled(10);

	/* Delete L00 and L01 entirely, from the start of the buffer. */
	deleteRange(buf, 0, 0, 0, 2, 0);

	TEST_ASSERT_EQUAL_STRING("L02", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(8, E.windows[1]->rowoff);
	/* The window still shows the same text, and its reported line
	 * number still names the row it is looking at. */
	TEST_ASSERT_EQUAL_STRING("L10", row_str(buf, E.windows[1]->rowoff));
	TEST_ASSERT_EQUAL_INT(10, E.windows[1]->cy);
}

void test_rowoff_tracks_text_across_insert_above(void) {
	struct buffer *buf = two_windows_scrolled(10);

	/* Two whole lines inserted above the viewport top. */
	buf->cx = 0;
	buf->cy = 0;
	insertNewline(2);

	TEST_ASSERT_EQUAL_INT(12, E.windows[1]->rowoff);
	TEST_ASSERT_EQUAL_STRING("L10", row_str(buf, E.windows[1]->rowoff));
	TEST_ASSERT_EQUAL_INT(14, E.windows[1]->cy);
}

void test_rowoff_inside_deleted_span_collapses_to_first_row(void) {
	struct buffer *buf = two_windows_scrolled(10);

	/* The deletion spans the viewport top: rows 8..12 collapse into
	 * one, so the top can only be the surviving row. */
	deleteRange(buf, 0, 8, 0, 12, 0);

	TEST_ASSERT_EQUAL_INT(8, E.windows[1]->rowoff);
	TEST_ASSERT_EQUAL_STRING("L12", row_str(buf, 8));
}

void test_rowoff_unchanged_by_edit_below_viewport_top(void) {
	struct buffer *buf = two_windows_scrolled(10);

	buf->cx = 0;
	buf->cy = 15;
	insertNewline(1);

	TEST_ASSERT_EQUAL_INT(10, E.windows[1]->rowoff);
	TEST_ASSERT_EQUAL_STRING("L10", row_str(buf, E.windows[1]->rowoff));
}

void test_rowoff_adjusted_for_focused_window_too(void) {
	struct buffer *buf = two_windows_scrolled(10);

	/* Focus the second window: cx/cy are not adjusted for a focused
	 * window because the command moves them, but nothing moves the
	 * viewport, so rowoff still must be. */
	E.windows[0]->focused = 0;
	E.windows[1]->focused = 1;

	deleteRange(buf, 0, 0, 0, 2, 0);

	TEST_ASSERT_EQUAL_INT(8, E.windows[1]->rowoff);
	TEST_ASSERT_EQUAL_STRING("L10", row_str(buf, E.windows[1]->rowoff));
}

/* Spec 05 P2, at the command level: C-SPC then typing.  In Emacs the
 * mark stays before the typed text, so the region is what was typed
 * and C-w kills it.  The mark used to move along with the insertion,
 * leaving an empty region. */
void test_mark_stays_before_typed_text(void) {
	const char *lines[] = { "xy", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 1;
	buf->cy = 0;
	processKeypress(CMD_SET_MARK);
	E.self_insert_key = 'a';
	processKeypress(CMD_SELF_INSERT);
	E.self_insert_key = 'b';
	processKeypress(CMD_SELF_INSERT);
	TEST_ASSERT_EQUAL_STRING("xaby", (char *)buf->row[0].chars);
	TEST_ASSERT_EQUAL_INT(1, buf->markx);
	TEST_ASSERT_EQUAL_INT(0, buf->marky);
	TEST_ASSERT_EQUAL_INT(3, buf->cx);

	/* So C-w takes exactly what was typed... */
	processKeypress(CMD_KILL_REGION);
	TEST_ASSERT_EQUAL_STRING("xy", (char *)buf->row[0].chars);

	/* ...and C-y gives it back. */
	buf->cx = 2;
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("xyab", (char *)buf->row[0].chars);
}

/* Spec 05 P2: another window's cursor, a register point and a ring
 * entry at the place where text is inserted all stay before it, as in
 * Emacs (window-point, point-to-register and the mark ring are all
 * markers there).
 *
 * Each is read back the way a user reaches it: C-u C-SPC through the
 * ring, C-x r j to the register, C-x o to the other window. */

/* Run `cmd` with one key waiting for it. */
static void pressWithKey(int cmd, int key) {
	int keys[] = { key };
	scriptKeys(keys, 1);
	muteStdout();
	processKeypress(cmd);
	unmuteStdout();
	TEST_ASSERT_EQUAL_INT(1, test_key_pos);
	clearKeys();
}

static void assertPoint(struct buffer *buf, int cx, int cy) {
	TEST_ASSERT_EQUAL_INT(cx, buf->cx);
	TEST_ASSERT_EQUAL_INT(cy, buf->cy);
}

void test_other_points_stay_before_inserted_text(void) {
	/* "b\nc" on the kill ring, copied with M-w out of a scratch
	 * buffer that is gone again before the test proper starts. */
	const char *bc[] = { "b", "c" };
	struct buffer *scratch = make_test_buffer_lines(bc, 2);
	scratch->markx = 0;
	scratch->marky = 0;
	scratch->mark_active = 1;
	scratch->cx = 1;
	scratch->cy = 1;
	processKeypress(CMD_COPY);
	destroyBuffer(scratch);
	E.buf = NULL;
	E.headbuf = NULL;
	E.windows[0]->buf = NULL;

	makeMinibuffer();
	const char *lines[] = { "xy", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 1;
	buf->cy = 0;
	processKeypress(CMD_CREATE_WINDOW); /* window 1 keeps (1,0) */
	processKeypress(CMD_SET_MARK);	    /* mark at (1,0) */
	buf->cx = 0;
	processKeypress(CMD_SET_MARK); /* the ring gets (1,0) */
	buf->cx = 1;
	pressWithKey(CMD_POINT_REGISTER, 'q'); /* register q: (1,0) */

	/* Type at (1,0), then yank two lines after it. */
	E.self_insert_key = 'a';
	processKeypress(CMD_SELF_INSERT);
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("xab", (char *)buf->row[0].chars);
	TEST_ASSERT_EQUAL_STRING("cy", (char *)buf->row[1].chars);
	/* The typing cursor itself is moved by the command. */
	assertPoint(buf, 1, 1);

	/* C-u C-SPC three times: the mark the yank set where its text
	 * went in, the mark from before the yank, then the ring entry.
	 * Had the entry moved with the text it would now be at (1,1),
	 * after the yanked newline. */
	E.uarg = 4;
	processKeypress(CMD_SET_MARK);
	assertPoint(buf, 2, 0);
	E.uarg = 4;
	processKeypress(CMD_SET_MARK);
	assertPoint(buf, 0, 0);
	E.uarg = 4;
	processKeypress(CMD_SET_MARK);
	assertPoint(buf, 1, 0);

	/* C-x r j q: the register point did not move either. */
	buf->cx = 0;
	buf->cy = 1;
	pressWithKey(CMD_JUMP_REGISTER, 'q');
	assertPoint(buf, 1, 0);

	/* C-x o: the other window's cursor is where it was. */
	buf->cx = 0;
	buf->cy = 1;
	processKeypress(CMD_OTHER_WINDOW);
	TEST_ASSERT(E.buf == buf);
	assertPoint(buf, 1, 0);

	freeMinibuffer();
}

int main(void) {
	TEST_BEGIN();

	/* Deletion */
	RUN_TEST(test_del_point_before_same_line);
	RUN_TEST(test_del_point_before_earlier_line);
	RUN_TEST(test_del_point_at_start);
	RUN_TEST(test_del_point_inside_single_line);
	RUN_TEST(test_del_point_inside_multiline);
	RUN_TEST(test_del_point_at_end);
	RUN_TEST(test_del_point_after_on_end_line);
	RUN_TEST(test_del_point_after_on_end_line_multiline);
	RUN_TEST(test_del_point_after_later_line);

	/* Insertion */
	RUN_TEST(test_ins_point_before_same_line);
	RUN_TEST(test_ins_point_before_earlier_line);
	RUN_TEST(test_ins_point_at_start_same_line);
	RUN_TEST(test_ins_point_at_start_multiline);
	RUN_TEST(test_ins_point_after_on_same_line);
	RUN_TEST(test_ins_multiline_point_on_start_line);
	RUN_TEST(test_ins_point_after_later_line);
	RUN_TEST(test_ins_point_after_single_line);

	/* Mark ring */
	RUN_TEST(test_markring_adjusted_on_delete);
	RUN_TEST(test_markring_adjusted_on_insert);
	RUN_TEST(test_markring_pop_jumps_to_right_line);
	RUN_TEST(test_markring_pop_stays_on_char_boundary);
	RUN_TEST(test_markring_pop_clamps_out_of_range_entry);
	RUN_TEST(test_markring_multiple_entries_all_adjusted);

	/* Viewport top */
	RUN_TEST(test_rowoff_tracks_text_across_delete_above);
	RUN_TEST(test_rowoff_tracks_text_across_insert_above);
	RUN_TEST(test_rowoff_inside_deleted_span_collapses_to_first_row);
	RUN_TEST(test_rowoff_unchanged_by_edit_below_viewport_top);
	RUN_TEST(test_rowoff_adjusted_for_focused_window_too);

	/* Spec 05 P2: points stay before text inserted at them */
	RUN_TEST(test_mark_stays_before_typed_text);
	RUN_TEST(test_other_points_stay_before_inserted_text);

	return TEST_END();
}
