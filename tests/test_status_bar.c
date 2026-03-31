/* test_status_bar.c — Characterisation tests for drawStatusBar.
 *
 * These pin the visible content of the status bar under various
 * buffer states.  They exist to catch regressions during the Phase 8
 * decomposition; the tests were written first and must stay green. */

#include "test.h"
#include "test_harness.h"
#include "display.h"
#include "abuf.h"
#include "message.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>

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
	drawStatusBar(E.windows[0], &ab, 1);
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

/* 1. Dirty + read-only flags show **% */
static void test_dirty_readonly_flags(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->dirty = 1;
	buf->read_only = 1;

	char *s = render_status();
	/* Flags field should contain **% */
	TEST_ASSERT(strstr(s, "**%") != NULL);
	free(s);
}

/* 2. Clean + writable flags show --- */
static void test_clean_flags(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->dirty = 0;
	buf->read_only = 0;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "-- ") != NULL);
	free(s);
}

/* 3. Rectangle mode: (Rect) does not appear — it was removed.
 *    Instead test word_wrap shows (Wrap). */
static void test_wrap_indicator(void) {
	struct buffer *buf = make_test_buffer("hello world");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->word_wrap = 1;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "(Wrap)") != NULL);
	free(s);
}

/* 4. Macro recording shows (Macro) */
static void test_macro_indicator(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	E.recording = 1;

	char *s = render_status();
	TEST_ASSERT(strstr(s, "(Macro)") != NULL);
	free(s);
}

/* 5. external_mod shows DISK CHANGED and preempts (Macro) */
static void test_disk_changed_preempts_macro(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->filename = xstrdup("test.c");
	computeDisplayNames();
	buf->external_mod = 1;
	E.recording = 1;

	char *s = render_status();
	/* Should contain the disk-changed warning */
	TEST_ASSERT(strstr(s, "FILE") != NULL ||
		    strstr(s, "MOD") != NULL);
	/* Should NOT contain (Macro) */
	TEST_ASSERT(strstr(s, "(Macro)") == NULL);
	free(s);
}

/* 6. Narrow screen: basename always visible even when name is long */
static void test_narrow_screen_shows_basename(void) {
	E.screencols = 40;
	struct buffer *buf = make_test_buffer("x");
	buf->filename = xstrdup("very/long/path/to/some/deep/file.c");
	computeDisplayNames();

	char *s = render_status();
	TEST_ASSERT(strstr(s, "file.c") != NULL);
	free(s);
}

/* 7. Line:col position shows in middle block */
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

/* 8. Status bar width matches screencols */
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

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_dirty_readonly_flags);
	RUN_TEST(test_clean_flags);
	RUN_TEST(test_wrap_indicator);
	RUN_TEST(test_macro_indicator);
	RUN_TEST(test_disk_changed_preempts_macro);
	RUN_TEST(test_narrow_screen_shows_basename);
	RUN_TEST(test_linecol_position);
	RUN_TEST(test_status_bar_width);
	return TEST_END();
}
