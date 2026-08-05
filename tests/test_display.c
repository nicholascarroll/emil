/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_display.c: Screen rendering, scrolling and window focus. */

#include "test.h"
#include "unicode.h"
#include "test_harness.h"
#include "display.h"
#include "window.h"
#include "abuf.h"
#include "edit.h"
#include "region.h"
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

/* ================= B6 — wrong-buffer mark check ==================
 *
 * computeRowHighlightBounds(buf, ...) reads everything from its `buf`
 * parameter except one call: markInvalidSilent(), which takes no
 * argument and consults the global E.buf.  So an unfocused window's
 * selection is drawn or not drawn according to whether the *focused*
 * buffer happens to have a valid mark.
 *
 * Both tests give buffer B the identical, valid, active selection.
 * Only the focused buffer A's mark differs between them, and B must
 * render the same way in both. */

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

/* Control: same B, but now A does have a valid mark.  This one passes
 * today — the pair is what shows the rendering depends on A. */
void test_b6_unfocused_selection_drawn_when_focused_has_mark(void) {
	struct buffer *a, *b;
	b6_setup(&a, &b);

	a->cx = 0;
	a->cy = 0;
	a->markx = 4;
	a->marky = 0;
	a->mark_active = 1;

	int len = 0;
	char *out = render_rows(E.windows[1], &len);
	int highlighted = containsBytes(out, len, "\x1b[7m", 4);
	free(out);

	TEST_ASSERT(highlighted);

	cleanupTestEditor();
}

/* ================= B10 — whatCursor reads window 0 =================
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
	static const char *lines[2] = {
		"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
		"abcdefghij"
	};
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
	invalidateScreenCache(buf);

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
	invalidateScreenCache(buf);

	scrollViewport(E.windows[0], buf, 1);

	TEST_ASSERT(E.windows[0]->rowoff >= 0);
	TEST_ASSERT(E.windows[0]->rowoff <= buf->numrows);
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
	RUN_TEST(test_b6_unfocused_selection_drawn_when_focused_has_mark);
	RUN_TEST(test_b10_whatcursor_uses_focused_window_rowoff);
	RUN_TEST(test_scroll_leaves_cursor_on_char_boundary);
	RUN_TEST(test_scroll_up_with_stale_rowoff_stays_in_bounds);
	RUN_TEST(test_scroll_down_with_stale_rowoff_stays_in_bounds);

	return TEST_END();
}
