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
#include <locale.h>

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

/* ---- The `Bot` indicator, in both wrap modes ----
 *
 * §5.1.2 says `Bot` when the end of the buffer is visible.  The end
 * flag had one formula per mode -- `rowoff + height > numrows` without
 * wrap, "does the last screen line fall within the window" with it --
 * and they disagree by one line at exactly this position: the last row
 * drawn on the window's last line, no blank line below it.  Without
 * wrap the bar reported a percentage there; with wrap, `Bot`.
 *
 * Ten five-column rows plus the buffer's own final empty row (§4.9) is
 * eleven screen lines in either mode, so a six-line window with its top
 * on row 5 ends exactly on row 10.  The two modes draw the identical
 * picture, which is the whole point: they must report it identically. */
static struct buffer *last_row_on_last_line(int wrap, int rowoff) {
	initTestEditor();
	const char *lines[10];
	for (int i = 0; i < 10; i++)
		lines[i] = "short";
	struct buffer *buf = make_test_buffer_lines(lines, 10);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->focused = 1;
	E.windows[0]->height = 6;
	E.windows[0]->rowoff = rowoff;
	E.windows[0]->skip_sublines = 0;
	buf->word_wrap = wrap;
	buf->cy = buf->numrows - 1;
	buf->cx = 0;
	return buf;
}

/* Draw the window, which is what sets buf->end, and report whether the
 * status bar names the given indicator. */
static int drawAndStatusHas(struct window *win, const char *needle) {
	struct abuf rows = ABUF_INIT;
	drawRows(win, &rows, win->height, E.screencols);
	abFree(&rows);

	struct abuf bar = ABUF_INIT;
	drawStatusBar(win, &bar, win->height + 1, -1);
	int found = containsBytes(bar.b, bar.len, needle, (int)strlen(needle));
	abFree(&bar);
	return found;
}

void test_bot_shown_when_the_last_row_is_on_the_windows_last_line(void) {
	for (int wrap = 0; wrap <= 1; wrap++) {
		struct buffer *buf = last_row_on_last_line(wrap, 5);
		TEST_ASSERT_EQUAL_INT(11, buf->numrows);

		TEST_ASSERT_TRUE(drawAndStatusHas(E.windows[0], "Bot"));
		TEST_ASSERT_EQUAL_INT(1, buf->end);
		cleanupTestEditor();
	}
}

/* The complement, so the test above cannot be satisfied by an end flag
 * that is simply always set: one line higher, row 10 is off screen. */
void test_bot_not_shown_when_the_last_row_is_off_screen(void) {
	for (int wrap = 0; wrap <= 1; wrap++) {
		struct buffer *buf = last_row_on_last_line(wrap, 4);

		TEST_ASSERT_FALSE(drawAndStatusHas(E.windows[0], "Bot"));
		TEST_ASSERT_EQUAL_INT(0, buf->end);
		cleanupTestEditor();
	}
}

/* And at the top of a buffer that fits entirely, `All` rather than
 * `Bot` -- the same flag, read together with rowoff == 0. */
void test_all_shown_when_the_whole_buffer_fits(void) {
	for (int wrap = 0; wrap <= 1; wrap++) {
		struct buffer *buf = last_row_on_last_line(wrap, 0);
		E.windows[0]->height = buf->numrows;

		TEST_ASSERT_TRUE(drawAndStatusHas(E.windows[0], "All"));
		TEST_ASSERT_EQUAL_INT(1, buf->end);
		cleanupTestEditor();
	}
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

	struct cursorHint hint;
	int cursor_col = scrollFocused(&hint);
	(void)cursor_col;
	int scx, scy;
	screenCursorPos(E.windows[0], &hint, &scx, &scy);

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

	struct cursorHint hint;
	int cursor_col = scrollFocused(&hint);
	(void)cursor_col;
	int scx, scy;
	screenCursorPos(E.windows[0], &hint, &scx, &scy);

	TEST_ASSERT_EQUAL_INT(0, scy);
	TEST_ASSERT_EQUAL_INT(0, scx);
	cleanupTestEditor();
}

/* The hint scrollFocused() hands to screenCursorPos() must be the
 * position screenCursorPos() would have computed for itself (#116).
 * A hint that disagrees puts the terminal cursor in the wrong cell,
 * with nothing in the frame to notice: the character under it is drawn
 * by drawRows, which never consults either path.  So the two are
 * asserted equal directly, over every row and a spread of columns, in
 * both wrap modes and at four window heights -- including zero, where
 * neither path has a real line to report. */
void test_cursor_hint_matches_the_computed_position(void) {
	static const int heights[] = { 0, 1, 2, 5 };
	for (int wrap = 0; wrap <= 1; wrap++)
	for (size_t h = 0; h < sizeof(heights) / sizeof(heights[0]); h++) {
		struct buffer *buf = wrapped_buffer(6, heights[h]);
		buf->word_wrap = wrap;

		for (int cy = 0; cy < buf->numrows; cy++) {
			int size = buf->row[cy].size;
			for (int cx = 0; cx <= size; cx += 37) {
				buf->cy = cy;
				buf->cx = cx;

				struct cursorHint hint;
				scrollFocused(&hint);

				int hx, hy, wx, wy;
				screenCursorPos(E.windows[0], &hint, &hx, &hy);
				screenCursorPos(E.windows[0], NULL, &wx, &wy);

				TEST_ASSERT_EQUAL_INT(wx, hx);
				TEST_ASSERT_EQUAL_INT(wy, hy);
				/* Including the degenerate window: no
				 * screen row is ever negative. */
				TEST_ASSERT(hy >= 0 && hx >= 0);
			}
		}
		cleanupTestEditor();
	}
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

/* ================================================================
 * DEF-2 (#117) — minibuffer height under-counted with wide characters
 *
 * Three places modelled the minibuffer's geometry and two of them
 * disagreed: refreshScreen sized it by dividing total columns by
 * screencols, while drawMinibuffer filled it character by character
 * and pushed a straddling wide character to the next line.  The
 * division assumed every line is filled to exactly screencols; each
 * short line cost one column, the error accumulated, and the tail of
 * the message was silently dropped.
 *
 * The invariant these pin: the number of lines the layout reports IS
 * the number drawMinibuffer needs, and no message byte is lost.
 * ================================================================ */

/* Repeat a CJK character (U+8A9E, 3 bytes / 2 columns) n times. */
static char *cjkRepeat(int n) {
	char *s = xmalloc(n * 3 + 1);
	for (int i = 0; i < n; i++)
		memcpy(s + i * 3, "\xE8\xAF\xAD", 3);
	s[n * 3] = '\0';
	return s;
}

/* The four cases the report measured, each an odd screencols where
 * the old division came up a line short. */
void test_minibuf_layout_sizes_wide_messages(void) {
	struct minibufLine lines[MINIBUF_MAX_LINES];
	struct {
		int screencols;
		int nchars;
		int expected;
	} cases[] = {
		{ 9, 9, 3 },
		{ 9, 13, 4 },
		{ 11, 11, 3 },
		{ 11, 16, 4 },
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char *msg = cjkRepeat(cases[i].nchars);
		int n = minibufLayout(msg, 0, cases[i].screencols, lines,
				      MINIBUF_MAX_LINES);
		TEST_ASSERT_EQUAL_INT(cases[i].expected, n);
		free(msg);
	}
}

/* No byte of the message may be dropped: the lines must tile it
 * exactly, in order, with no gaps and no overlap. */
void test_minibuf_layout_covers_every_byte(void) {
	struct minibufLine lines[MINIBUF_MAX_LINES];

	for (int cols = 3; cols <= 13; cols++) {
		for (int nchars = 1; nchars <= 12; nchars++) {
			char *msg = cjkRepeat(nchars);
			int n = minibufLayout(msg, 0, cols, lines,
					      MINIBUF_MAX_LINES);
			TEST_ASSERT_TRUE(n >= 1);

			int expect_start = 0;
			for (int i = 0; i < n; i++) {
				TEST_ASSERT_EQUAL_INT(expect_start,
						      lines[i].start);
				TEST_ASSERT_TRUE(lines[i].end >
						 lines[i].start);
				TEST_ASSERT_TRUE(lines[i].cols <= cols);
				expect_start = lines[i].end;
			}
			/* Fully consumed unless the 5-line cap bit. */
			if (n < MINIBUF_MAX_LINES)
				TEST_ASSERT_EQUAL_INT((int)strlen(msg),
						      expect_start);
			free(msg);
		}
	}
}

/* Sizing and drawing must agree.  drawMinibuffer emits one "\r\n"
 * between lines, so a height matching the layout produces exactly
 * (n-1) of them and carries every message byte. */
void test_minibuf_drawn_lines_match_the_sizing(void) {
	struct minibufLine lines[MINIBUF_MAX_LINES];
	E.screencols = 9;

	char *msg = cjkRepeat(9); /* the first DEF-2 case */
	snprintf(E.statusmsg, sizeof(E.statusmsg), "%s", msg);
	E.statusmsg_show = 1;

	int n = minibufLayout(E.statusmsg, 0, E.screencols, lines,
			      MINIBUF_MAX_LINES);
	minibuffer_height = n;

	struct abuf ab = ABUF_INIT;
	drawMinibuffer(&ab);

	int newlines = 0;
	for (int i = 0; i + 1 < ab.len; i++)
		if (ab.b[i] == '\r' && ab.b[i + 1] == '\n')
			newlines++;
	TEST_ASSERT_EQUAL_INT(n - 1, newlines);

	/* Every byte of the message reached the frame: the tail is
	 * what the old sizing dropped. */
	TEST_ASSERT_TRUE(containsBytes(ab.b, ab.len, msg + 6 * 3, 3 * 3));

	abFree(&ab);
	free(msg);
	E.statusmsg[0] = '\0';
	E.statusmsg_show = 0;
	minibuffer_height = 1;
}

/* ASCII must be unchanged: bytes == columns, so the old division was
 * already exact there and the rewrite must reproduce it. */
void test_minibuf_layout_ascii_unchanged(void) {
	struct minibufLine lines[MINIBUF_MAX_LINES];

	char msg[41];
	memset(msg, 'a', 40);
	msg[40] = '\0';

	/* 40 columns over a 10-column screen = 4 lines. */
	TEST_ASSERT_EQUAL_INT(4, minibufLayout(msg, 0, 10, lines,
					       MINIBUF_MAX_LINES));
	/* With a 3-column prefix on line 0: 7 + 10 + 10 + 10 = 37,
	 * so a fifth line is needed and the cap holds it at 5. */
	TEST_ASSERT_EQUAL_INT(5, minibufLayout(msg, 3, 10, lines,
					       MINIBUF_MAX_LINES));
	TEST_ASSERT_EQUAL_INT(7, lines[0].cols);

	/* A message that fits is one line. */
	TEST_ASSERT_EQUAL_INT(1, minibufLayout("short", 0, 40, lines,
					       MINIBUF_MAX_LINES));
	TEST_ASSERT_EQUAL_INT(5, lines[0].cols);

	/* An empty message still occupies one line. */
	TEST_ASSERT_EQUAL_INT(1, minibufLayout("", 0, 40, lines,
					       MINIBUF_MAX_LINES));
	TEST_ASSERT_EQUAL_INT(0, lines[0].cols);
}

/* A wide character on a 1-column screen cannot fit, and must still
 * advance: no stall, no infinite layout. */
void test_minibuf_layout_forces_progress(void) {
	struct minibufLine lines[MINIBUF_MAX_LINES];
	char *msg = cjkRepeat(3);

	int n = minibufLayout(msg, 0, 1, lines, MINIBUF_MAX_LINES);
	TEST_ASSERT_EQUAL_INT(3, n);
	for (int i = 0; i < n; i++)
		TEST_ASSERT_EQUAL_INT(3, lines[i].end - lines[i].start);

	free(msg);
}

/* Sizing and drawing must agree.  This runs the SIZING path
 * (minibufHeightNeeded, which is what refreshScreen calls) against
 * what drawMinibuffer emits — asserting the layout against itself
 * would not have caught DEF-2, since the defect was one model
 * disagreeing with another.
 *
 * Every one of the report's four cases is checked, because the
 * under-count accumulates and a single case could pass by luck. */
void test_minibuf_sizing_matches_what_is_drawn(void) {
	struct {
		int screencols;
		int nchars;
		int expected;
	} cases[] = {
		{ 9, 9, 3 },
		{ 9, 13, 4 },
		{ 11, 11, 3 },
		{ 11, 16, 4 },
	};

	for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		char *msg = cjkRepeat(cases[i].nchars);
		E.screencols = cases[i].screencols;
		snprintf(E.statusmsg, sizeof(E.statusmsg), "%s", msg);
		E.statusmsg_show = 1;

		/* The sizing path, as refreshScreen runs it. */
		int sized = minibufHeightNeeded();
		TEST_ASSERT_EQUAL_INT(cases[i].expected, sized);

		minibuffer_height = sized;
		struct abuf ab = ABUF_INIT;
		drawMinibuffer(&ab);

		/* drawMinibuffer emits one "\r\n" between lines. */
		int newlines = 0;
		for (int j = 0; j + 1 < ab.len; j++)
			if (ab.b[j] == '\r' && ab.b[j + 1] == '\n')
				newlines++;
		TEST_ASSERT_EQUAL_INT(sized - 1, newlines);

		/* The message's LAST character reached the frame.  This
		 * is the byte the under-count dropped. */
		TEST_ASSERT_TRUE(containsBytes(ab.b, ab.len,
					       msg + (cases[i].nchars - 1) * 3,
					       3));

		abFree(&ab);
		free(msg);
	}

	E.statusmsg[0] = '\0';
	E.statusmsg_show = 0;
	minibuffer_height = 1;
}

void tearDown(void) {
}

int main(void) {
	/* charAdvance prices a CJK character at 2 columns only under a
	 * UTF-8 LC_CTYPE (§1.3); the DEF-2 cases are all about wide
	 * characters, so they need it. */
	setlocale(LC_CTYPE, "C.UTF-8");

	TEST_BEGIN();

	RUN_TEST(test_b6_unfocused_selection_drawn_when_focused_has_no_mark);
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

	RUN_TEST(test_cursor_hint_matches_the_computed_position);

	/* The `Bot` indicator, both wrap modes */
	RUN_TEST(test_bot_shown_when_the_last_row_is_on_the_windows_last_line);
	RUN_TEST(test_bot_not_shown_when_the_last_row_is_off_screen);
	RUN_TEST(test_all_shown_when_the_whole_buffer_fits);

	/* §D.4 */
	RUN_TEST(test_recenter_centres_in_screen_lines_under_wrap);
	RUN_TEST(test_recenter_unchanged_without_wrap);

	/* Window-relative viewport assertions */
	RUN_TEST(test_scroll_to_end_of_buffer_with_wrap_stops_at_the_bottom);
	RUN_TEST(test_scroll_puts_a_cursor_below_the_window_on_its_last_line);
	RUN_TEST(test_scroll_puts_a_cursor_above_the_window_on_its_first_line);
	RUN_TEST(
		test_viewport_stable_across_an_edit_above_a_non_focused_window);

	/* DEF-2 — one minibuffer layout model */
	RUN_TEST(test_minibuf_layout_sizes_wide_messages);
	RUN_TEST(test_minibuf_layout_covers_every_byte);
	RUN_TEST(test_minibuf_drawn_lines_match_the_sizing);
	RUN_TEST(test_minibuf_sizing_matches_what_is_drawn);
	RUN_TEST(test_minibuf_layout_ascii_unchanged);
	RUN_TEST(test_minibuf_layout_forces_progress);

	return TEST_END();
}
