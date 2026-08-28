/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_status_bar.c: Characterisation tests for drawStatusBar.
 */

#include "test.h"
#include "unicode.h"
#include "test_harness.h"
#include "display.h"
#include "abuf.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <locale.h>

/* Strip ANSI escape sequences and the leading CSI cursor-position
 * sequence from the abuf, leaving only visible characters.
 * Also strips the trailing \x1b[m\r\n reset. */
static char *strip_escapes(const struct abuf *ab) {
	char *out = xmalloc(ab->len + 1);
	int oi = 0;
	int i = 0;
	while (i < ab->len) {
		if (ab->b[i] == '\x1b') {
			/* Skip ESC [ ... <letter> */
			i++;
			if (i < ab->len && ab->b[i] == '[') {
				i++;
				while (i < ab->len &&
				       !((ab->b[i] >= 'A' && ab->b[i] <= 'Z') ||
					 (ab->b[i] >= 'a' && ab->b[i] <= 'z')))
					i++;
				if (i < ab->len) i++; /* skip final letter */
			}
		} else if (ab->b[i] == '\r' || ab->b[i] == '\n') {
			i++;
		} else {
			out[oi++] = ab->b[i++];
		}
	}
	out[oi] = '\0';
	return out;
}

/* Render the status bar for the current E.buf into a string.
 * Caller frees. */
static char *render_status(void) {
	struct abuf ab = ABUF_INIT;
	drawStatusBar(E.windows[0], &ab, 1, -1);
	char *visible = strip_escapes(&ab);
	abFree(&ab);
	return visible;
}

void setUp(void) {
	memset(&E, 0, sizeof(E));
	initTestEditor();
}

void tearDown(void) {
	cleanupTestEditor();
}

/* The two-character flag field: left column is read-only, right is
 * modified, each doubling the other when it has nothing to say.
 *
 *   --   clean, writable
 *   **   modified, writable
 *   %%   clean, read-only
 *   %*   modified, read-only
 */

/* Dirty + read-only shows %* */
static void test_dirty_readonly_flags(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->dirty = 1;
	buf->read_only = 1;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "test.c %*") != NULL);
	free(s);
}

/* Clean + writable shows -- */
static void test_clean_flags(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->dirty = 0;
	buf->read_only = 0;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "test.c --") != NULL);
	free(s);
}

/* Dirty + writable shows ** */
static void test_dirty_writable_flags(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->dirty = 1;
	buf->read_only = 0;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "test.c **") != NULL);
	free(s);
}

/* Clean + read-only shows %% */
static void test_clean_readonly_flags(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->dirty = 0;
	buf->read_only = 1;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "test.c %%") != NULL);
	free(s);
}

/* Word wrap shows (Wrap) in the right block. */
static void test_wrap_indicator(void) {
	struct buffer *buf = make_test_buffer("hello world");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->word_wrap = 1;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "(Wrap)") != NULL);
	free(s);
}

/* Macro recording shows (Macro). */
static void test_macro_indicator(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	E.recording = 1;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "(Macro)") != NULL);
	free(s);
}

/* A warning outranks a mode indicator: external_mod lights
 * FILE MODIFIED and suppresses (Macro). */
static void test_disk_changed_preempts_macro(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->external_mod = 1;
	E.recording = 1;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "FILE MODIFIED") != NULL);
	TEST_ASSERT(strstr(s, "(Macro)") == NULL);
	free(s);
}

/* min_name_len is a hard floor: the basename survives a squeeze. */
static void test_narrow_screen_shows_basename(void) {
	E.screencols = 40;
	struct buffer *buf = make_test_buffer("x");
	buf->filename = xstrdup("very/long/path/to/some/deep/file.c");
	computeDisplayNames();

	char *s = render_status();
	TEST_ASSERT(strstr(s, "file.c") != NULL);
	free(s);
}

/* The middle block shows line:col, 1-indexed row, 0-indexed column. */
static void test_linecol_position(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->cx = 3;
	buf->cy = 0;

	char *s = render_status();
	/* Should show 1:3 (1-indexed row, 0-indexed col) */
	TEST_ASSERT(strstr(s, "1:3") != NULL);
	free(s);
}

/* The column is a DISPLAY column, not a byte offset.
 *
 * Row is "\ta\x01b日c".  Byte offset and display column diverge from
 * the first character on: a tab is one byte but eight cells, a control
 * character is one byte but two (rendered ^A), and 日 is three bytes
 * but two cells.
 *
 *   byte   0    1   2    3   4       7
 *   char   \t   a   ^A   b   日      c
 *   col    0    8   9    11  12      14
 *
 * Every offset asserted below differs from its own display column, so
 * a regression to printing cx raw cannot pass any of them by
 * coincidence.  The wide-character case needs an LC_CTYPE locale under
 * which wcwidth() is meaningful; main() sets one, as test_cjk_indic.c
 * does. */
static void test_linecol_is_display_column(void) {
	struct buffer *buf = make_test_buffer("\ta\x01" "b\xE6\x97\xA5"
					      "c");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->cy = 0;

	buf->cx = 1; /* after the tab: byte 1, cell 8 */
	char *s = render_status();
	TEST_ASSERT(strstr(s, "1:8") != NULL);
	free(s);

	buf->cx = 3; /* after ^A: byte 3, cell 11 */
	s = render_status();
	TEST_ASSERT(strstr(s, "1:11") != NULL);
	free(s);

	buf->cx = 4; /* after 'b': byte 4, cell 12 */
	s = render_status();
	TEST_ASSERT(strstr(s, "1:12") != NULL);
	free(s);

	buf->cx = 7; /* after 日: byte 7, cell 14 */
	s = render_status();
	TEST_ASSERT(strstr(s, "1:14") != NULL);
	TEST_ASSERT(strstr(s, "1:7") == NULL);
	free(s);

	buf->cx = 8; /* end of line: byte 8, cell 15 */
	s = render_status();
	TEST_ASSERT(strstr(s, "1:15") != NULL);
	free(s);
}

/* Plain ASCII must be unchanged: byte offset and display column
 * coincide, so the conversion must be invisible on the common case. */
static void test_linecol_ascii_unchanged(void) {
	struct buffer *buf = make_test_buffer("hello world");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->cx = 7;
	buf->cy = 0;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "1:7") != NULL);
	free(s);
}

/* The scroll percentage must not overflow.
 *
 * `rowoff * 100` was computed in int.  rowoff can reach numrows, whose
 * ceiling is INT_MAX / 2, so the product overflows above ~21.4 M rows
 * -- signed overflow, undefined, and reached before the division can
 * bring it back into range.  Not hypothetical at emil's 1 GiB ceiling:
 * a 1 GiB file of 40-byte lines is ~26 M rows.
 *
 * numrows and rowoff are set directly rather than by building 26 M
 * real rows, which would cost ~624 MB and minutes.  The percentage
 * branch reads only these two fields, so the arithmetic under test is
 * the arithmetic that runs; nothing here depends on rows existing
 * beyond row[0], which does.
 *
 * Unfixed, this prints "-69%": the product wraps to -1794967296 and
 * the division carries the sign through.  So the test fails loudly at
 * -O2 as well as trapping under UBSan. */
static void test_scroll_percent_no_overflow(void) {
	struct buffer *buf = make_test_buffer("x");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();

	int real_numrows = buf->numrows;
	buf->numrows = 26000000; /* ~1 GiB of 40-byte lines */
	buf->end = 0;
	E.windows[0]->rowoff = 25000000;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "96%") != NULL);
	/* The specific wrong value, not "no minus sign anywhere": an
	 * unmodified buffer's flag string is "--". */
	TEST_ASSERT(strstr(s, "-69%") == NULL);
	free(s);

	/* Restore the real count, not a hardcoded 1: newBuffer() seeds a
	 * trailing empty row, so this buffer has two, and destroyBuffer()
	 * frees numrows of them.  Getting this wrong leaks the second
	 * row's byte -- which is how LeakSanitizer found it. */
	buf->numrows = real_numrows;
	E.windows[0]->rowoff = 0;
}

/* The bar fills exactly screencols cells. */
static void test_status_bar_width(void) {
	E.screencols = 60;
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();

	char *s = render_status();
	int len = (int)strlen(s);
	TEST_ASSERT_EQUAL(60, len);
	free(s);
}


/* Render the status bar for a window.  Caller frees. */
static char *render_status_win(struct window *win, int *len_out) {
	struct abuf ab = ABUF_INIT;
	drawStatusBar(win, &ab, 1, -1);
	char *out = xmalloc(ab.len + 1);
	memcpy(out, ab.b, ab.len);
	out[ab.len] = '\0';
	if (len_out)
		*len_out = ab.len;
	abFree(&ab);
	return out;
}

/* Strip ANSI escapes, leaving visible bytes (multi-byte UTF-8 intact). */
static char *strip_escapes_n(const char *in, int len, int *out_len) {
	char *out = xmalloc(len + 1);
	int oi = 0, i = 0;
	while (i < len) {
		if (in[i] == '\x1b') {
			i++;
			if (i < len && in[i] == '[') {
				i++;
				while (i < len &&
				       !((in[i] >= 'A' && in[i] <= 'Z') ||
					 (in[i] >= 'a' && in[i] <= 'z')))
					i++;
				if (i < len)
					i++;
			}
		} else if (in[i] == '\r' || in[i] == '\n') {
			i++;
		} else {
			out[oi++] = in[i++];
		}
	}
	out[oi] = '\0';
	if (out_len)
		*out_len = oi;
	return out;
}

/* B12 — statusLeft truncates mid-character
 *
 * `snprintf(trunc, ..., "...%s", dname + dlen - tail)` is byte
 * arithmetic against a *column* budget, so the tail pointer can land
 * inside a multi-byte sequence and emit invalid UTF-8 to the terminal.
 * truncateToCols() exists a few lines up and does this correctly;
 * statusRight uses it, statusLeft does not. */

void test_b12_statusleft_truncation_stays_valid_utf8(void) {
	struct buffer *buf = make_test_buffer("content");

	/* A long CJK name: every character is 3 bytes, so a byte-based
	 * left-truncation lands mid-sequence for most widths. */
	free(buf->filename);
	buf->filename = xstrdup("/"
				"\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
				"\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
				"\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
				"\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
				".txt");

	/* Narrow enough to force the truncation branch. */
	E.screencols = 40;

	int len = 0;
	char *raw = render_status_win(E.windows[0], &len);
	int vis_len = 0;
	char *vis = strip_escapes_n(raw, len, &vis_len);

	TEST_ASSERT_EQUAL_INT(1, utf8_validate((const uint8_t *)vis,
					       vis_len));

	free(vis);
	free(raw);
}

/* B13 — statusLeft returns snprintf's length
 *
 * `left` is char[512]; statusLeft returns snprintf's would-be length,
 * and the caller does abAppend(ab, left, left_len).  With screencols
 * over ~508 and a long name, that reads past the end of the stack
 * buffer.  Latent, but one line to clamp.
 *
 * The read is out of bounds rather than wrong-valued, so the reliable
 * signal is a sanitizer; without one, the observable symptom is a
 * status bar longer than the screen. */

void test_b13_statusleft_does_not_overrun_its_buffer(void) {
	struct buffer *buf = make_test_buffer("content");

	/* The name has to be longer than statusLeft's 512-byte output
	 * buffer but still SHORTER than name_width, or the "..." branch
	 * truncates it and nothing overflows.  550 bytes with a 600
	 * column screen sits in that window: name_width works out to
	 * 566, so no truncation, and snprintf returns 554 against a
	 * cap of 512.  No '/' in the name, so min_name == dlen. */
	char longname[551];
	memset(longname, 'x', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	free(buf->filename);
	buf->filename = xstrdup(longname);

	E.screencols = 600;

	int len = 0;
	char *raw = render_status_win(E.windows[0], &len);

	/* snprintf NUL-terminates at 511; returning its would-be length
	 * makes the caller append the terminator and then whatever
	 * happened to be on the stack after it.  A status bar never
	 * legitimately contains a NUL byte, so this is a reliable
	 * signal without needing a sanitizer -- though ASan also
	 * reports the read as a stack-buffer-overflow. */
	TEST_ASSERT_NULL(memchr(raw, '\0', len));

	free(raw);
}

/* DEF-1 (#117) — leftTruncate split multi-byte characters
 *
 * The B12 defect's twin.  The 0.9.3 CHANGELOG fix repaired statusLeft;
 * this copy of the loop — the one that produces display_name itself,
 * plus completion lists and save/open/kill messages — kept measuring
 * a column budget in bytes, so `s + (len - tail)` landed mid-sequence
 * at two CJK widths in three. */

void test_left_truncate_valid_utf8_at_every_width(void) {
	/* The report's reproduction: "/home/u/" + 9 CJK + ".txt".
	 * 39 bytes, 30 columns; byte arithmetic against a column
	 * budget forced invalid output at widths 12, 14, 15, 17, 18,
	 * 20, 21 and split characters silently elsewhere. */
	const char *name = "/home/u/"
			   "\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
			   "\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
			   "\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
			   ".txt";

	for (int w = 1; w <= 32; w++) {
		char *r = leftTruncate(name, w);
		TEST_ASSERT_EQUAL_INT(1, utf8_validate((const uint8_t *)r,
						       (int)strlen(r)));
		TEST_ASSERT_TRUE(stringWidth((const uint8_t *)r) <= w);
		free(r);
	}

	/* One exact expectation, at a width the old code corrupted:
	 * budget 12 leaves 9 columns after "...", which holds the
	 * rightmost 2 CJK (4 cols) + ".txt" (4 cols). */
	char *r = leftTruncate(name, 12);
	TEST_ASSERT_EQUAL_STRING("...\xE8\xAF\xAD\xE8\xAF\xAD.txt", r);
	free(r);
}

void test_left_truncate_ascii_unchanged(void) {
	/* Bytes == columns for ASCII, so the column rewrite must
	 * reproduce the historical results exactly. */
	char *r = leftTruncate("short", 10);
	TEST_ASSERT_EQUAL_STRING("short", r);
	free(r);

	r = leftTruncate("abcdefghij", 8);
	TEST_ASSERT_EQUAL_STRING("...fghij", r);
	free(r);
}

void test_left_truncate_tiny_budget_keeps_whole_chars(void) {
	/* max_width <= 3 has no room for "..." and used to return a
	 * raw byte tail — also mid-character for CJK.  Now: the bare
	 * rightmost whole characters that fit. */
	const char *cjk = "\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"; /* 6 cols */
	char *r = leftTruncate(cjk, 2);
	TEST_ASSERT_EQUAL_STRING("\xE8\xAF\xAD", r);
	free(r);

	r = leftTruncate(cjk, 3); /* two chars = 4 cols > 3: still one */
	TEST_ASSERT_EQUAL_STRING("\xE8\xAF\xAD", r);
	free(r);
}

int main(void) {
	/* wcwidth() only reports 2 for a CJK character under a UTF-8
	 * LC_CTYPE; without this the display-column test below would
	 * measure 日 as one cell.  Same call, same reason, as
	 * test_cjk_indic.c.  No test in this file depends on the C
	 * locale's widths. */
	setlocale(LC_CTYPE, "C.UTF-8");

	TEST_BEGIN();
	RUN_TEST(test_dirty_readonly_flags);
	RUN_TEST(test_clean_flags);
	RUN_TEST(test_dirty_writable_flags);
	RUN_TEST(test_clean_readonly_flags);
	RUN_TEST(test_wrap_indicator);
	RUN_TEST(test_macro_indicator);
	RUN_TEST(test_disk_changed_preempts_macro);
	RUN_TEST(test_narrow_screen_shows_basename);
	RUN_TEST(test_linecol_position);
	RUN_TEST(test_linecol_is_display_column);
	RUN_TEST(test_linecol_ascii_unchanged);
	RUN_TEST(test_scroll_percent_no_overflow);
	RUN_TEST(test_status_bar_width);

	RUN_TEST(test_b12_statusleft_truncation_stays_valid_utf8);
	RUN_TEST(test_b13_statusleft_does_not_overrun_its_buffer);

	RUN_TEST(test_left_truncate_valid_utf8_at_every_width);
	RUN_TEST(test_left_truncate_ascii_unchanged);
	RUN_TEST(test_left_truncate_tiny_budget_keeps_whole_chars);
	return TEST_END();
}
