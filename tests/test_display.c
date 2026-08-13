/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_display.c: Screen rendering, scrolling and window focus. */

#include "test.h"
#include "unicode.h"
#include "test_harness.h"
#include "display.h"
#include "wrap.h"
#include "window.h"
#include "abuf.h"
#include "edit.h"
#include "region.h"
#include "motion.h"
#include "util.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* Portable substring search over a byte range (the haystack contains
 * escape sequences, not a NUL-terminated string, and memmem is a GNU
 * extension). */
static int containsBytes(const char *hay, int hay_len, const char *needle,
			 int needle_len) {
	for (int i = 0; i + needle_len <= hay_len; i++) {
		if (memcmp(hay + i, needle, needle_len) == 0)
			return 1;
	}
	return 0;
}

/* Render a window's text area and return the raw bytes (escapes kept,
 * because the region highlight IS an escape sequence).  Caller frees. */
static char *render_rows(struct window *win, int *len_out) {
	struct abuf ab = ABUF_INIT;
	drawRows(win, &ab, E.screenrows, E.screencols);
	char *out = xmalloc(ab.len + 1);
	memcpy(out, ab.b, ab.len);
	out[ab.len] = '\0';
	if (len_out)
		*len_out = ab.len;
	abFree(&ab);
	return out;
}

/* B6 — wrong-buffer mark check
 *
 * computeRowHighlightBounds(buf, ...) reads everything from its `buf`
 * parameter except one call: markInvalidSilent(), which takes no
 * argument and consults the global E.buf.  So an unfocused window's
 * selection is drawn or not drawn according to whether the *focused*
 * buffer happens to have a valid mark.
 *
 * Buffer B carries a valid, active selection throughout; the focused
 * buffer A carries none.  B must still be highlighted. */

static void b6_setup(struct buffer **a_out, struct buffer **b_out) {
	initTestEditor();

	/* Second window, showing buffer B, unfocused. */
	E.windows = realloc(E.windows, 2 * sizeof(struct window *));
	E.windows[1] = calloc(1, sizeof(struct window));
	E.nwindows = 2;
	E.windows[0]->focused = 1;
	E.windows[1]->focused = 0;
	E.windows[0]->height = 10;
	E.windows[1]->height = 10;

	struct buffer *a = make_test_buffer("aaaa focused buffer");

	struct buffer *b = newBuffer();
	insertRow(b, 0, (const uint8_t *)"bbbb selected text", 18);
	b->cx = 0;
	b->cy = 0;
	b->dirty = 0;
	clearUndosAndRedos(b);
	/* A valid, active, non-empty region on B. */
	b->markx = 10;
	b->marky = 0;
	b->mark_active = 1;

	b->next = E.headbuf;
	E.headbuf = b;

	E.windows[1]->buf = b;
	E.buf = a;
	E.windows[0]->buf = a;

	*a_out = a;
	*b_out = b;
}

/* Focused buffer A has NO mark.  B's own selection is valid, so B must
 * still be highlighted. */
void test_b6_unfocused_selection_drawn_when_focused_has_no_mark(void) {
	struct buffer *a, *b;
	b6_setup(&a, &b);

	a->markx = -1;
	a->marky = -1;
	a->mark_active = 0;

	int len = 0;
	char *out = render_rows(E.windows[1], &len);
	int highlighted = containsBytes(out, len, "\x1b[7m", 4);
	free(out);

	TEST_ASSERT(highlighted);

	cleanupTestEditor();
}

/* B10 — whatCursor reads window 0
 *
 * `int screen_y = E.buf->cy - E.windows[0]->rowoff + 1;` — hardcoded
 * to the first window, so C-x = reports the wrong screen row whenever
 * focus is anywhere else. */

void test_b10_whatcursor_uses_focused_window_rowoff(void) {
	initTestEditor();

	E.windows = realloc(E.windows, 2 * sizeof(struct window *));
	E.windows[1] = calloc(1, sizeof(struct window));
	E.nwindows = 2;
	E.windows[0]->focused = 0;
	E.windows[0]->rowoff = 0;
	E.windows[1]->focused = 1;
	E.windows[1]->rowoff = 10;

	const char *lines[20];
	for (int i = 0; i < 20; i++)
		lines[i] = "some line of text";
	struct buffer *buf = make_test_buffer_lines(lines, 20);
	E.windows[1]->buf = buf;
	buf->cy = 12;
	buf->cx = 0;

	whatCursor();

	/* Focused window starts at row 10, so cy 12 is screen row 3. */
	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "screen:3,"));

	cleanupTestEditor();
}

/* Scrolling used to carry the cursor's byte offset onto a different row
 * and clamp it only against that row's byte length, so scrolling from a
 * long ASCII line onto a line of multibyte text left the cursor inside a
 * character.  The fuzzer reported this as "cursor is mid-character" and
 * it was the only failure category it had.
 *
 * Driven through processKeypress rather than by calling scrollLineUp
 * directly, because the rule is enforced by clampPositions at the end
 * of every command -- the invariant holds at command boundaries, not at
 * every instant inside one. */
void test_scroll_leaves_cursor_on_char_boundary(void) {
	initTestEditor();
	/* Row 0 is three 3-byte characters; row 1 is ASCII and longer,
	 * so a byte offset legal on row 1 falls inside a character on
	 * row 0. */
	static const char *lines[2] = { "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
					"abcdefghij" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 1;
	E.windows[0]->rowoff = 1;

	buf->cy = 1;
	buf->cx = 7; /* inside row 0's third character */
	processKeypress(CMD_SCROLL_UP);

	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT(buf->cx == buf->row[0].size ||
		    !utf8_isCont(buf->row[0].chars[buf->cx]));
	cleanupTestEditor();
}

/* Stale rowoff in scrollViewport.
 *
 * Deleting rows does not adjust any window's rowoff, so between the
 * edit and the next frame a window can name a row that no longer
 * exists.  refreshScreen clamps it, but a keyboard macro or a
 * uarg-repeated command runs many operations per frame.  The word-wrap
 * path in scrollViewport indexes buf->row[win->rowoff] directly, so a
 * stale rowoff there read a row->chars that delRow had already freed
 * (heap-use-after-free, found by fuzz_undo.c under ASan).
 *
 * Asserted as a bounds check rather than left to the sanitizer, so the
 * test fails on a plain build too. */
void test_scroll_up_with_stale_rowoff_stays_in_bounds(void) {
	initTestEditor();
	static const char *lines[5] = { "alpha", "beta", "gamma", "delta",
					"epsilon" };
	struct buffer *buf = make_test_buffer_lines(lines, 5);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 2;
	buf->word_wrap = 1;

	/* Scrolled near the bottom, then the buffer shrinks under us. */
	E.windows[0]->rowoff = 4;
	E.windows[0]->skip_sublines = 0;
	while (buf->numrows > 2)
		delRow(buf, buf->numrows - 1);
	buf->cy = 0;
	buf->cx = 0;

	scrollViewport(E.windows[0], buf, -1);

	TEST_ASSERT(E.windows[0]->rowoff >= 0);
	TEST_ASSERT(E.windows[0]->rowoff < buf->numrows);
	cleanupTestEditor();
}

/* The same clamp on the scroll-down side, so the guard is not quietly
 * tied to one direction.
 *
 * Word wrap only, and the bound here is rowoff <= numrows rather than
 * rowoff < numrows.  Scrolling down deliberately walks rowoff one past
 * the last row so the final line can leave the screen -- both branches
 * do it, and refreshScreen pulls it back to numrows - 1 before drawing.
 * What the entry clamp guarantees is that rowoff cannot still be the
 * stale value it arrived with; every deref inside the scroll-down loop
 * is already guarded by its own rowoff >= numrows test.  The strict
 * bound is asserted on the scroll-up side above, which is where the
 * unguarded buf->row[win->rowoff] lives. */
void test_scroll_down_with_stale_rowoff_stays_in_bounds(void) {
	initTestEditor();
	static const char *lines[5] = { "alpha", "beta", "gamma", "delta",
					"epsilon" };
	struct buffer *buf = make_test_buffer_lines(lines, 5);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 2;
	buf->word_wrap = 1;

	E.windows[0]->rowoff = 4;
	E.windows[0]->skip_sublines = 0;
	while (buf->numrows > 2)
		delRow(buf, buf->numrows - 1);
	buf->cy = 0;
	buf->cx = 0;

	scrollViewport(E.windows[0], buf, 1);

	TEST_ASSERT(E.windows[0]->rowoff >= 0);
	TEST_ASSERT(E.windows[0]->rowoff <= buf->numrows);
	cleanupTestEditor();
}

/* clampCursorToViewport derives cy from the window's rowoff and
 * height, neither of which is bounded by the buffer, and then indexes
 * buf->row[cy].  A window sized to zero rows -- reachable in the real
 * editor, because neither the height distribution nor sizePopupWindow
 * floors at one row -- makes the first branch assign cy = rowoff
 * outright, so a rowoff past a shortened buffer reads off the end of
 * the row array.  Found by fuzz_undo on seeds other than 1. */
void test_clamp_cursor_zero_height_window_stays_in_bounds(void) {
	initTestEditor();
	static const char *lines[5] = { "alpha", "beta", "gamma", "delta",
					"epsilon" };
	struct buffer *buf = make_test_buffer_lines(lines, 5);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 0;
	E.windows[0]->rowoff = 4;
	buf->word_wrap = 0;

	while (buf->numrows > 2)
		delRow(buf, buf->numrows - 1);
	buf->cy = 0;
	buf->cx = 0;

	clampCursorToViewport(E.windows[0], buf);

	TEST_ASSERT(buf->cy >= 0);
	TEST_ASSERT(buf->cy < buf->numrows);
	TEST_ASSERT(buf->cx <= buf->row[buf->cy].size);
	cleanupTestEditor();
}

/* The same guarantee through the call path the fuzzer actually
 * reported: pageDown -> clampCursorToViewport -> row[cy].  Driving
 * the command rather than the helper keeps the test honest if the
 * clamping ever moves between the two. */
void test_page_down_zero_height_window_stays_in_bounds(void) {
	initTestEditor();
	static const char *lines[6] = { "alpha", "beta",  "gamma",
					"delta", "epsil", "zeta" };
	struct buffer *buf = make_test_buffer_lines(lines, 6);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 0;
	E.windows[0]->rowoff = 5;
	buf->word_wrap = 0;

	while (buf->numrows > 2)
		delRow(buf, buf->numrows - 1);
	buf->cy = 0;
	buf->cx = 0;

	pageDown(1);

	TEST_ASSERT(buf->cy >= 0);
	TEST_ASSERT(buf->cy < buf->numrows);
	TEST_ASSERT(buf->cx <= buf->row[buf->cy].size);
	cleanupTestEditor();
}

/* ---- §C2: the frame stops computing full row widths ----
 *
 * A row far wider than the viewport was walked end to end every frame
 * to decide where padding starts, and again to report the status bar
 * column.  Both answers are now produced by work the frame already
 * does, so nothing asks the row for its total width.
 *
 * cached_width is the observable: calculateLineWidth() is the only
 * thing that populates it, so a row still holding -1 after a frame is
 * proof that no full-row walk happened.  Asserting on elapsed time
 * would test the machine. */

static struct buffer *wide_row_buffer(void) {
	initTestEditor();
	static char wide[5001];
	memset(wide, 'a', 5000);
	wide[5000] = '\0';
	const char *lines[1] = { wide };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 4;
	buf->word_wrap = 0;
	buf->row[0].cached_width = -1;
	return buf;
}

void test_drawrows_wide_row_does_not_compute_full_width(void) {
	struct buffer *buf = wide_row_buffer();
	buf->cx = 0;
	buf->cy = 0;

	free(render_rows(E.windows[0], NULL));

	TEST_ASSERT_EQUAL_INT(-1, buf->row[0].cached_width);
	cleanupTestEditor();
}

/* The padding decision still has to be right: a row narrower than the
 * viewport is filled to the full width with spaces, and no \x1b[K is
 * emitted (it would erase the last column under pending-wrap). */
void test_drawrows_pads_short_row_to_full_width(void) {
	initTestEditor();
	static const char *lines[1] = { "abc" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 1;
	E.screenrows = 1; /* render_rows draws E.screenrows lines */
	buf->word_wrap = 0;

	int len = 0;
	char *out = render_rows(E.windows[0], &len);

	TEST_ASSERT_EQUAL_INT(E.screencols, len);
	TEST_ASSERT_EQUAL_INT(0, memcmp(out, "abc", 3));
	int spaces = 1;
	for (int i = 3; i < len; i++)
		if (out[i] != ' ')
			spaces = 0;
	TEST_ASSERT(spaces);
	free(out);
	cleanupTestEditor();
}

/* The status bar takes the column scroll() already computed.  A hint
 * the row's own text could not produce shows it is used rather than
 * recomputed -- and the row is left uncached, so nothing walked it. */
void test_statusbar_uses_the_frames_cursor_column(void) {
	struct buffer *buf = wide_row_buffer();
	buf->cx = buf->row[0].size;
	buf->cy = 0;

	struct abuf ab = ABUF_INIT;
	drawStatusBar(E.windows[0], &ab, 1, 4242);
	int found = containsBytes(ab.b, ab.len, "1:4242", 6);
	abFree(&ab);

	TEST_ASSERT(found);
	TEST_ASSERT_EQUAL_INT(-1, buf->row[0].cached_width);
	cleanupTestEditor();
}

/* And the hint must be the number the status bar would have computed,
 * or the bar reports a wrong column with no way to notice.  A tab and
 * a wide character make byte offset and display column diverge. */
void test_scroll_returns_the_cursor_display_column(void) {
	initTestEditor();
	static const char *lines[1] = { "a\tb\xe6\x97\xa5"
					"c" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 4;
	buf->word_wrap = 0;
	buf->cy = 0;

	for (int cx = 0; cx <= buf->row[0].size; cx++) {
		if (cx < buf->row[0].size && utf8_isCont(buf->row[0].chars[cx]))
			continue;
		buf->cx = cx;
		int expected = charsToDisplayColumn(&buf->row[0], cx);
		TEST_ASSERT_EQUAL_INT(expected, scroll());
	}
	cleanupTestEditor();
}

/* ---- §D.4: C-l centres in screen lines ----
 *
 * recenter subtracted half the window height from cy, which is half a
 * window of *rows*.  Under wrap a row is several screen lines, so the
 * cursor landed far below centre -- and the further down a wrapped
 * buffer, the worse.  Walking back height/2 screen lines puts it on
 * the middle line by construction, in both modes. */
void test_recenter_centres_in_screen_lines_under_wrap(void) {
	initTestEditor();
	/* Each row is 200 columns: three sub-lines at 80 columns. */
	static char wide[201];
	memset(wide, 'a', 200);
	wide[200] = '\0';
	const char *lines[10];
	for (int i = 0; i < 10; i++)
		lines[i] = wide;
	struct buffer *buf = make_test_buffer_lines(lines, 10);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 10;
	buf->word_wrap = 1;
	buf->cy = 5;
	buf->cx = 0;

	recenter(E.windows[0]);

	/* Five screen lines back from (row 5, sub 0): 4.2, 4.1, 4.0,
	 * 3.2, 3.1.  The row-count form landed on row 0 instead, which
	 * is fifteen screen lines up. */
	TEST_ASSERT_EQUAL_INT(3, E.windows[0]->rowoff);
	TEST_ASSERT_EQUAL_INT(1, E.windows[0]->skip_sublines);
	cleanupTestEditor();
}

/* With wrap off a row is a screen line, so the walk must land exactly
 * where the subtraction did. */
void test_recenter_unchanged_without_wrap(void) {
	initTestEditor();
	const char *lines[20];
	for (int i = 0; i < 20; i++)
		lines[i] = "short line";
	struct buffer *buf = make_test_buffer_lines(lines, 20);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 10;
	buf->word_wrap = 0;
	buf->cy = 12;
	buf->cx = 0;

	recenter(E.windows[0]);

	TEST_ASSERT_EQUAL_INT(7, E.windows[0]->rowoff);
	TEST_ASSERT_EQUAL_INT(0, E.windows[0]->skip_sublines);
	cleanupTestEditor();
}

/* ---- Viewport assertions, stated relative to the window ----
 *
 * These name distances from the window's own top -- which screen line
 * the cursor lands on, which text the first line shows -- rather than
 * a screen line counted from the start of the buffer.  An absolute
 * index is exactly what #111 removed, so a suite that asserted on one
 * would be pinning a quantity the editor no longer computes. */

/* Ten rows of 200 columns: three sub-lines each at 80 columns. */
static struct buffer *wrapped_buffer(int rows, int height) {
	initTestEditor();
	static char wide[201];
	memset(wide, 'a', 200);
	wide[200] = '\0';
	const char *lines[32];
	for (int i = 0; i < rows && i < 32; i++)
		lines[i] = wide;
	struct buffer *buf = make_test_buffer_lines(lines, rows);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = height;
	E.windows[0]->focused = 1;
	buf->word_wrap = 1;
	return buf;
}

/* Scrolling down stops with two blank lines showing, counted in screen
 * lines.  Ten rows of three sub-lines plus the buffer's own final empty
 * row (§4.9) is 31 screen lines; a ten-line window therefore stops with
 * its top on screen line 23, which is row 7's third sub-line.  Asking
 * for more must not move it. */
void test_scroll_to_end_of_buffer_with_wrap_stops_at_the_bottom(void) {
	struct buffer *buf = wrapped_buffer(10, 10);
	TEST_ASSERT_EQUAL_INT(11, buf->numrows);

	scrollViewport(E.windows[0], buf, 100);
	TEST_ASSERT_EQUAL_INT(7, E.windows[0]->rowoff);
	TEST_ASSERT_EQUAL_INT(2, E.windows[0]->skip_sublines);

	scrollViewport(E.windows[0], buf, 100);
	TEST_ASSERT_EQUAL_INT(7, E.windows[0]->rowoff);
	TEST_ASSERT_EQUAL_INT(2, E.windows[0]->skip_sublines);
	cleanupTestEditor();
}

/* A cursor below the window brings the viewport to it, landing on the
 * window's last screen line.  Stated as a distance from the top, which
 * is the only thing the renderer needs to know. */
void test_scroll_puts_a_cursor_below_the_window_on_its_last_line(void) {
	struct buffer *buf = wrapped_buffer(10, 10);
	E.windows[0]->rowoff = 0;
	E.windows[0]->skip_sublines = 0;
	buf->cy = 5; /* nine sub-lines below the window */
	buf->cx = 0;

	int cursor_col = scroll();
	int scx, scy;
	screenCursorPos(E.windows[0], cursor_col, &scx, &scy);

	TEST_ASSERT_EQUAL_INT(E.windows[0]->height - 1, scy);
	TEST_ASSERT_EQUAL_INT(0, scx);
	cleanupTestEditor();
}

/* And a cursor above it becomes the top line. */
void test_scroll_puts_a_cursor_above_the_window_on_its_first_line(void) {
	struct buffer *buf = wrapped_buffer(10, 10);
	E.windows[0]->rowoff = 6;
	E.windows[0]->skip_sublines = 2;
	buf->cy = 1;
	buf->cx = 0;

	int cursor_col = scroll();
	int scx, scy;
	screenCursorPos(E.windows[0], cursor_col, &scx, &scy);

	TEST_ASSERT_EQUAL_INT(0, scy);
	TEST_ASSERT_EQUAL_INT(0, scx);
	cleanupTestEditor();
}

/* An edit above a non-focused window's top must leave that window
 * showing the same text.  4.1 asserts the anchor moves; this asserts
 * what the user sees, which is the thing that was wrong. */
void test_viewport_stable_across_an_edit_above_a_non_focused_window(void) {
	initTestEditor();
	E.windows = xrealloc(E.windows, 2 * sizeof(struct window *));
	E.windows[1] = xcalloc(1, sizeof(struct window));
	E.nwindows = 2;
	E.windows[0]->focused = 1;
	E.windows[1]->focused = 0;
	E.windows[0]->height = 5;
	E.windows[1]->height = 5;
	E.screenrows = 5;

	static char names[20][16];
	const char *lines[20];
	for (int i = 0; i < 20; i++) {
		snprintf(names[i], sizeof(names[i]), "L%02d", i);
		lines[i] = names[i];
	}
	struct buffer *buf = make_test_buffer_lines(lines, 20);
	E.windows[1]->buf = buf;
	E.windows[1]->rowoff = 10;

	int len = 0;
	char *before = render_rows(E.windows[1], &len);
	TEST_ASSERT_EQUAL_INT(0, memcmp(before, "L10", 3));
	free(before);

	/* Two whole lines inserted above that window's top. */
	buf->cx = 0;
	buf->cy = 0;
	insertNewline(2);

	char *after = render_rows(E.windows[1], &len);
	TEST_ASSERT_EQUAL_INT(0, memcmp(after, "L10", 3));
	free(after);
	cleanupTestEditor();
}

/* These tests manage the editor themselves. */
void setUp(void) {
}

void tearDown(void) {
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_b6_unfocused_selection_drawn_when_focused_has_no_mark);
	RUN_TEST(test_b10_whatcursor_uses_focused_window_rowoff);
	RUN_TEST(test_scroll_leaves_cursor_on_char_boundary);
	RUN_TEST(test_scroll_up_with_stale_rowoff_stays_in_bounds);
	RUN_TEST(test_scroll_down_with_stale_rowoff_stays_in_bounds);
	RUN_TEST(test_clamp_cursor_zero_height_window_stays_in_bounds);
	RUN_TEST(test_page_down_zero_height_window_stays_in_bounds);

	/* §C2 */
	RUN_TEST(test_drawrows_wide_row_does_not_compute_full_width);
	RUN_TEST(test_drawrows_pads_short_row_to_full_width);
	RUN_TEST(test_statusbar_uses_the_frames_cursor_column);
	RUN_TEST(test_scroll_returns_the_cursor_display_column);

	/* §D.4 */
	RUN_TEST(test_recenter_centres_in_screen_lines_under_wrap);
	RUN_TEST(test_recenter_unchanged_without_wrap);

	/* Window-relative viewport assertions */
	RUN_TEST(test_scroll_to_end_of_buffer_with_wrap_stops_at_the_bottom);
	RUN_TEST(test_scroll_puts_a_cursor_below_the_window_on_its_last_line);
	RUN_TEST(test_scroll_puts_a_cursor_above_the_window_on_its_first_line);
	RUN_TEST(
		test_viewport_stable_across_an_edit_above_a_non_focused_window);

	return TEST_END();
}
