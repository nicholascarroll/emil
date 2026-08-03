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
	/* Insert 5 chars at (5,3), ending at (10,3), point at (5,3) */
	int px = 5, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(10, px); /* shifted right by 5 */
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

	return TEST_END();
}
