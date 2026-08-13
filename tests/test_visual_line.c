/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_visual_line.c: Visual line movement, start/end, kill in wrap mode. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include "display.h"
#include "wrap.h"
#include <stdint.h>

/* ---- Helper ---- */

static erow make_row(const char *s) {
	erow r;
	memset(&r, 0, sizeof(r));
	r.size = strlen(s);
	r.chars = (uint8_t *)s;
	r.cached_width = -1;
	return r;
}

/* ---- displayColumnToByteOffset tests ---- */

void test_dcbo_simple_ascii(void) {
	erow row = make_row("Hello, world! This is a long line.");
	/* On a 20-col screen, sub-line 0 starts at byte 0 */
	int b = displayColumnToByteOffset(&row, 20, 0, 0);
	TEST_ASSERT_EQUAL_INT(0, b);
	b = displayColumnToByteOffset(&row, 20, 0, 5);
	TEST_ASSERT_EQUAL_INT(5, b);
}

void test_dcbo_second_subline(void) {
	/* 40 'a' chars on a 20-col screen = 2 sub-lines */
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	erow row = make_row(buf);
	/* Sub-line 1 starts at byte 20 */
	int b = displayColumnToByteOffset(&row, 20, 1, 0);
	TEST_ASSERT_EQUAL_INT(20, b);
	b = displayColumnToByteOffset(&row, 20, 1, 5);
	TEST_ASSERT_EQUAL_INT(25, b);
}

void test_dcbo_clamp_to_subline_end(void) {
	/* "Hello" on 20-col screen: only 1 sub-line, 5 chars */
	erow row = make_row("Hello");
	/* target_col=99 should clamp to byte 5 (end of row) */
	int b = displayColumnToByteOffset(&row, 20, 0, 99);
	TEST_ASSERT_EQUAL_INT(5, b);
}

void test_dcbo_nonexistent_subline(void) {
	erow row = make_row("Hello");
	/* Sub-line 1 doesn't exist on a short line */
	int b = displayColumnToByteOffset(&row, 20, 1, 0);
	TEST_ASSERT_EQUAL_INT(5, b); /* clamped to row->size */
}

void test_dcbo_empty_row(void) {
	erow row = make_row("");
	int b = displayColumnToByteOffset(&row, 20, 0, 0);
	TEST_ASSERT_EQUAL_INT(0, b);
}

void test_dcbo_with_tab(void) {
	erow row = make_row("\tHello");
	/* Tab at col 0 expands to col 8. target_col=0 -> byte 0 (the tab) */
	int b = displayColumnToByteOffset(&row, 80, 0, 0);
	TEST_ASSERT_EQUAL_INT(0, b);
	/* target_col=8 -> byte 1 ('H') */
	b = displayColumnToByteOffset(&row, 80, 0, 8);
	TEST_ASSERT_EQUAL_INT(1, b);
}

/* ---- sublineBounds tests ---- */

void test_subline_bounds_single_line(void) {
	erow row = make_row("Hello");
	int sb, eb;
	int ok = sublineBounds(&row, 80, 0, &sb, &eb);
	TEST_ASSERT_TRUE(ok);
	TEST_ASSERT_EQUAL_INT(0, sb);
	TEST_ASSERT_EQUAL_INT(5, eb); /* row->size for last subline */
}

void test_subline_bounds_wrapped(void) {
	/* 40 'a' chars, 20-col screen => 2 sub-lines */
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	erow row = make_row(buf);
	int sb, eb;

	/* Sub-line 0: bytes 0..19 */
	int ok = sublineBounds(&row, 20, 0, &sb, &eb);
	TEST_ASSERT_TRUE(ok);
	TEST_ASSERT_EQUAL_INT(0, sb);
	TEST_ASSERT_EQUAL_INT(20, eb);

	/* Sub-line 1: bytes 20..39 */
	ok = sublineBounds(&row, 20, 1, &sb, &eb);
	TEST_ASSERT_TRUE(ok);
	TEST_ASSERT_EQUAL_INT(20, sb);
	TEST_ASSERT_EQUAL_INT(40, eb);
}

void test_subline_bounds_nonexistent(void) {
	erow row = make_row("Hello");
	int sb, eb;
	int ok = sublineBounds(&row, 80, 1, &sb, &eb);
	TEST_ASSERT_FALSE(ok);
}

/* ---- moveVisualRow (via moveCursor) integration ---- */

void test_visual_move_down_within_row(void) {
	/* 40 'a' chars on 20-col screen. Cursor at byte 5, sub-line 0. */
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 5;
	b->cy = 0;

	moveCursor(KEY_ARROW_DOWN, 1);
	/* Should move to sub-line 1, col 5 => byte 25 */
	TEST_ASSERT_EQUAL_INT(0, b->cy); /* same logical row */
	TEST_ASSERT_EQUAL_INT(25, b->cx);
}

void test_visual_move_up_within_row(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 25; /* sub-line 1, col 5 */
	b->cy = 0;

	moveCursor(KEY_ARROW_UP, 1);
	/* Should move to sub-line 0, col 5 => byte 5 */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(5, b->cx);
}

void test_visual_move_down_crosses_row(void) {
	/* Two rows: row 0 is short, row 1 has content */
	const char *lines[] = { "Hello", "World" };
	struct buffer *b = make_test_buffer_lines(lines, 2);
	E.screencols = 80;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 3;
	b->cy = 0;

	moveCursor(KEY_ARROW_DOWN, 1);
	TEST_ASSERT_EQUAL_INT(1, b->cy);
	TEST_ASSERT_EQUAL_INT(3, b->cx);
}

void test_visual_move_up_crosses_row(void) {
	/* Row 0: 40 'a' on 20-col = 2 sublines. Row 1: "Hello" */
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	const char *lines[] = { buf, "Hello" };
	struct buffer *b = make_test_buffer_lines(lines, 2);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cy = 1;
	b->cx = 3; /* col 3 on row 1 */

	moveCursor(KEY_ARROW_UP, 1);
	/* Should go to row 0, last sub-line (1), col 3 => byte 23 */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(23, b->cx);
}

void test_visual_move_down_at_buffer_end(void) {
	struct buffer *b = make_test_buffer("Hello");
	E.screencols = 80;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 3;
	b->cy = 0;

	moveCursor(KEY_ARROW_DOWN, 1);
	/* Should move to virtual line past EOF */
	TEST_ASSERT_EQUAL_INT(1, b->cy);
	TEST_ASSERT_EQUAL_INT(0, b->cx);
}

void test_visual_move_up_at_buffer_start(void) {
	struct buffer *b = make_test_buffer("Hello");
	E.screencols = 80;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 3;
	b->cy = 0;

	moveCursor(KEY_ARROW_UP, 1);
	/* Should stay put */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(3, b->cx);
}

/* ---- C-a / C-e visual line tests ---- */

void test_beginning_of_visual_line(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 25; /* sub-line 1, col 5 */
	b->cy = 0;

	beginningOfLine();
	/* Should go to start of sub-line 1 = byte 20 */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(20, b->cx);
}

void test_beginning_of_visual_line_first_subline(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 5; /* sub-line 0 */
	b->cy = 0;

	beginningOfLine();
	TEST_ASSERT_EQUAL_INT(0, b->cx); /* byte 0 */
}

void test_end_of_visual_line(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 5; /* sub-line 0, col 5 */
	b->cy = 0;

	endOfLine(0);
	/* Should go to last char of sub-line 0 = byte 19 (last 'a'
	 * before the sub-line break at byte 20) */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(19, b->cx);
}

void test_end_of_visual_line_last_subline(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 25; /* sub-line 1 */
	b->cy = 0;

	endOfLine(0);
	/* Should go to end of sub-line 1 = byte 40 = row->size */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(40, b->cx);
}

/* ---- C-a / C-e without wrap (regression) ---- */

void test_beginning_of_line_no_wrap(void) {
	struct buffer *b = make_test_buffer("Hello, world!");
	E.screencols = 80;
	b->word_wrap = 0;
	b->cx = 7;

	beginningOfLine();
	TEST_ASSERT_EQUAL_INT(0, b->cx);
}

void test_end_of_line_no_wrap(void) {
	struct buffer *b = make_test_buffer("Hello, world!");
	E.screencols = 80;
	b->word_wrap = 0;
	b->cx = 0;

	endOfLine(0);
	TEST_ASSERT_EQUAL_INT(13, b->cx);
}

/* ---- C-k visual line kill ---- */

void test_kill_visual_line_mid_subline(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 5; /* sub-line 0, col 5 */
	b->cy = 0;

	killLine(0);
	/* Should kill bytes 5..19 (15 chars from sub-line 0)
	 * Remaining: 5 'a' + 20 'a' from sub-line 1 = 25 'a' */
	TEST_ASSERT_EQUAL_INT(25, b->row[0].size);
	TEST_ASSERT_EQUAL_INT(5, b->cx);
}

void test_kill_line_no_wrap(void) {
	struct buffer *b = make_test_buffer("Hello, world!");
	E.screencols = 80;
	b->word_wrap = 0;
	b->cx = 5;

	killLine(0);
	/* Should kill from byte 5 to end of line */
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(b, 0));
}

/* ---- Runner ---- */

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- scrollViewport on an empty wrap buffer ---- */

void test_scroll_viewport_empty_wrap_buffer(void) {
	/* PageDown on an empty word-wrap buffer */
	struct buffer *b = make_test_buffer("");
	TEST_ASSERT_EQUAL_INT(1, b->numrows);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;

	scrollViewport(E.windows[0], b, 5);
	TEST_ASSERT_EQUAL_INT(0, E.windows[0]->rowoff);
	TEST_ASSERT_EQUAL_INT(0, E.windows[0]->skip_sublines);

	scrollViewport(E.windows[0], b, -3);
	TEST_ASSERT_EQUAL_INT(0, E.windows[0]->rowoff);
	TEST_ASSERT_EQUAL_INT(0, E.windows[0]->skip_sublines);
}

/* ---- charsToDisplayColumn caching boundary ---- */

/* char_pos == row->size is "the whole row" and must take the cached
 * calculateLineWidth() path.  A `>` here instead of `>=` sends a cursor
 * at end of line down the whole-row walk every frame.  Asserting the
 * cache was *populated* is what distinguishes the two paths -- both
 * return the same number. */
void test_ctdc_at_row_size_uses_cache(void) {
	erow row = make_row("hello\tworld");
	TEST_ASSERT_EQUAL_INT(-1, row.cached_width);
	int w = charsToDisplayColumn(&row, row.size);
	/* Order matters: calculateLineWidth() would populate the cache
	 * itself, so the cache must be inspected before it is called. */
	TEST_ASSERT_EQUAL_INT(w, row.cached_width);
	TEST_ASSERT_EQUAL_INT(calculateLineWidth(&row), w);
}

void test_ctdc_past_row_size_uses_cache(void) {
	erow row = make_row("hello\tworld");
	int w = charsToDisplayColumn(&row, row.size + 100);
	TEST_ASSERT_EQUAL_INT(w, row.cached_width);
	TEST_ASSERT_EQUAL_INT(calculateLineWidth(&row), w);
}

/* The walk now delegates to nextScreenX().  These pin the agreement
 * between the partial walk and the whole-row width it must build up
 * to, for tabs, control characters and wide characters. */
void test_ctdc_partial_matches_full(void) {
	erow row = make_row("ab\tc\x01"
			    "d\xe6\x97\xa5"
			    "e");
	int full = calculateLineWidth(&row);
	erow scratch = make_row("ab\tc\x01"
				"d\xe6\x97\xa5"
				"e");
	TEST_ASSERT_EQUAL_INT(full, charsToDisplayColumn(&scratch, row.size));
	TEST_ASSERT_EQUAL_INT(0, charsToDisplayColumn(&scratch, 0));
	TEST_ASSERT_EQUAL_INT(2, charsToDisplayColumn(&scratch, 2));
	/* byte 2 is a tab: column advances to the next tab stop */
	TEST_ASSERT_EQUAL_INT(EMIL_TAB_STOP, charsToDisplayColumn(&scratch, 3));
	/* byte 4 is \x01, a control character, rendered as ^A */
	TEST_ASSERT_EQUAL_INT(EMIL_TAB_STOP + 3,
			      charsToDisplayColumn(&scratch, 5));
}

void test_ctdc_empty_row(void) {
	erow row = make_row("");
	TEST_ASSERT_EQUAL_INT(0, charsToDisplayColumn(&row, 0));
	TEST_ASSERT_EQUAL_INT(0, charsToDisplayColumn(&row, 5));
}

/* The four tests above assert the cache is *populated*.  All four
 * would still pass if the cache were never invalidated -- which is
 * the failure §4.10 exists to prevent, and the one routing
 * char_pos >= row->size through the cache makes worse: the status bar
 * and the cursor placement read the cache every frame with the cursor
 * at end of line, so a missed invalidation renders wrongly
 * continuously instead of on a rare path.
 *
 * So assert what the protocol promises rather than what the cache
 * contains.  selfInsert()/delChar() rather than insertChar(): the
 * latter is a raw primitive that calls rowInsertChar() directly and
 * invalidates on the line below its own memmove, which is hard to get
 * wrong.  A keystroke goes through the mutation layer to
 * bulkInsert/bulkDelete, which is where an invalidation would actually
 * go missing, and that is the path worth pinning. */
void test_ctdc_width_follows_a_mutation(void) {
	struct buffer *b = make_test_buffer("abc");

	int w = charsToDisplayColumn(&b->row[0], b->row[0].size);
	TEST_ASSERT_EQUAL_INT(3, w);
	/* The cache is warm from here on, so a stale read is possible
	 * and the assertions below can distinguish one. */
	TEST_ASSERT_EQUAL_INT(3, b->row[0].cached_width);

	b->cx = 3;
	b->cy = 0;
	selfInsert(b, '\t', 1); /* "abc\t" -- one tab stop wide */
	w = charsToDisplayColumn(&b->row[0], b->row[0].size);
	TEST_ASSERT_EQUAL_INT(EMIL_TAB_STOP, w);

	b->cx = 3;
	delChar(1); /* back to "abc" */
	w = charsToDisplayColumn(&b->row[0], b->row[0].size);
	TEST_ASSERT_EQUAL_INT(3, w);
}

/* §4.10's other half is gone with the field it protected.
 *
 * cached_sublines was a remembered wrap count, and the test that stood
 * here drove the protocol keeping it honest: mutate, warm the width
 * from a display path, and check the sub-line count had not survived.
 * There is nothing to keep honest now -- the count is computed from the
 * row at the moment it is wanted (#108) -- so what is worth pinning is
 * that it stays derived, with no invalidation call anywhere in sight. */
void test_sublines_are_derived_not_remembered(void) {
	struct buffer *b = make_test_buffer("abcde");
	b->word_wrap = 1;

	TEST_ASSERT_EQUAL_INT(1, countScreenLines(&b->row[0], 10));

	b->cx = 5;
	b->cy = 0;
	selfInsert(b, 'x', 8); /* "abcdexxxxxxxx" -- 13 cols, wraps */

	TEST_ASSERT_EQUAL_INT(2, countScreenLines(&b->row[0], 10));
}

int main(void) {
	TEST_BEGIN();

	/* charsToDisplayColumn */
	RUN_TEST(test_ctdc_at_row_size_uses_cache);
	RUN_TEST(test_ctdc_past_row_size_uses_cache);
	RUN_TEST(test_ctdc_partial_matches_full);
	RUN_TEST(test_ctdc_empty_row);
	RUN_TEST(test_ctdc_width_follows_a_mutation);
	RUN_TEST(test_sublines_are_derived_not_remembered);

	/* displayColumnToByteOffset */
	RUN_TEST(test_dcbo_simple_ascii);
	RUN_TEST(test_dcbo_second_subline);
	RUN_TEST(test_dcbo_clamp_to_subline_end);
	RUN_TEST(test_dcbo_nonexistent_subline);
	RUN_TEST(test_dcbo_empty_row);
	RUN_TEST(test_dcbo_with_tab);

	/* sublineBounds */
	RUN_TEST(test_subline_bounds_single_line);
	RUN_TEST(test_subline_bounds_wrapped);
	RUN_TEST(test_subline_bounds_nonexistent);

	/* Visual row movement */
	RUN_TEST(test_visual_move_down_within_row);
	RUN_TEST(test_visual_move_up_within_row);
	RUN_TEST(test_visual_move_down_crosses_row);
	RUN_TEST(test_visual_move_up_crosses_row);
	RUN_TEST(test_visual_move_down_at_buffer_end);
	RUN_TEST(test_visual_move_up_at_buffer_start);

	/* C-a / C-e visual line */
	RUN_TEST(test_beginning_of_visual_line);
	RUN_TEST(test_beginning_of_visual_line_first_subline);
	RUN_TEST(test_end_of_visual_line);
	RUN_TEST(test_end_of_visual_line_last_subline);
	RUN_TEST(test_beginning_of_line_no_wrap);
	RUN_TEST(test_end_of_line_no_wrap);

	/* C-k visual line kill */
	RUN_TEST(test_kill_visual_line_mid_subline);
	RUN_TEST(test_kill_line_no_wrap);

	/* Empty-buffer scrolling */
	RUN_TEST(test_scroll_viewport_empty_wrap_buffer);

	return TEST_END();
}
