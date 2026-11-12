/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_register_preview.c: no row, and no drawing of a row, may run
 * past the end of an incomplete UTF-8 sequence.
 *
 * The *Register Preview* (C-x r v) showed each text register with
 * "%.60s", which cuts by bytes.  When byte 60 fell just after the lead
 * byte of a multibyte character the preview row ended in an incomplete
 * sequence, and renderLineWithHighlighting copied the full length the
 * lead byte announces: 1-2 bytes past the row's allocation
 * (heap-buffer-overflow under `make sanitize`; in a release build the
 * row's NUL and a stray heap byte went to the terminal).  Reachable
 * with a CJK line, C-SPC C-e C-x r s a, C-x r v.
 *
 * The preview now cuts on a character boundary, and the renderer and
 * utf8ColsToBytes (the minibuffer layout) never step past the end of
 * the bytes they were given.  Found by the pre-1.0 keystroke fuzzer
 * (tests/fuzz_keys.c). */

#include "test.h"
#include "test_harness.h"
#include "display.h"
#include "keymap.h"
#include <string.h>
#include "buffer.h"

/* Issue a command with `keys` waiting for whatever it reads. */
static void pressWithKeys(int cmd, const int *keys, int n) {
	scriptKeys(keys, n);
	muteStdout();
	processKeypress(cmd);
	unmuteStdout();
	TEST_ASSERT_EQUAL_INT(n, test_key_pos);
	clearKeys();
}

/* The buffer a window is showing under `name`, or NULL. */
static struct buffer *shownBuffer(const char *name) {
	for (int i = 0; i < E.nwindows; i++) {
		struct buffer *b = E.windows[i]->buf;
		if (b && b->filename && strcmp(b->filename, name) == 0)
			return b;
	}
	return NULL;
}

/* "ab" then 25 x U+65E5: the 60-byte cut falls one byte into the 20th
 * character. */
static char *cjkText(void) {
	char *s = xmalloc(2 + 25 * 3 + 1);
	strcpy(s, "ab");
	for (int i = 0; i < 25; i++)
		strcat(s, "\xe6\x97\xa5");
	return s;
}

void test_register_preview_cuts_on_character_boundary(void) {
	initTestEditor();
	makeMinibuffer();
	char *text = cjkText();
	const char *lines[] = { text };
	struct buffer *buf = make_test_buffer_lines(lines, 1);

	/* Select the line and save it: C-SPC C-e C-x r s a. */
	buf->markx = 0;
	buf->marky = 0;
	buf->mark_active = 1;
	buf->cx = buf->row[0].size;
	buf->cy = 0;
	int name[] = { 'a' };
	pressWithKeys(CMD_REGION_REGISTER, name, 1);

	/* C-x r v, then C-g at the register prompt.  The last frame
	 * drawn is the one showing the preview. */
	int quit[] = { KEY_CTRL('g') };
	pressWithKeys(CMD_VIEW_REGISTER, quit, 1);

	/* Before the fix the frame carried the row's NUL terminator
	 * (and a byte past it) after the dangling lead byte. */
	TEST_ASSERT(E.render_buf.len > 0);
	TEST_ASSERT(memchr(E.render_buf.b, '\0', E.render_buf.len) == NULL);
	TEST_ASSERT(utf8_validate((const uint8_t *)E.render_buf.b,
				  E.render_buf.len));

	free(text);
	freeMinibuffer();
	cleanupTestEditor();
}

/* Whatever produced it, a row ending in an incomplete sequence draws
 * without reading past the row. */
void test_render_incomplete_sequence_at_row_end(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "ab\xe6" };
	make_test_buffer_lines(lines, 1);

	muteStdout();
	refreshScreen();
	unmuteStdout();

	TEST_ASSERT(E.render_buf.len > 0);
	TEST_ASSERT(memchr(E.render_buf.b, '\0', E.render_buf.len) == NULL);

	freeMinibuffer();
	cleanupTestEditor();
}

/* The minibuffer layout walks the status message the same way; its
 * byte spans must end inside the message. */
void test_cols_to_bytes_stops_at_span_end(void) {
	const uint8_t *s = (const uint8_t *)"ab\xe6";
	int used = 0;
	TEST_ASSERT_EQUAL_INT(3, utf8ColsToBytes(s, 0, 3, 80, &used));

	struct minibufLine l[4];
	int n = minibufLayout("ab\xe6", 0, 80, l, 4);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_INT(3, l[0].end);
}

void setUp(void) {
}
void tearDown(void) {
}

/* C-x r V showed a point register's filename raw.  A filename can hold a
 * newline, and '\\n' is emil's row separator, never a byte in a row. */
void test_register_point_filename_newline(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "text" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	free(buf->filename);
	buf->filename = xstrdup("a\nb\xe6\x97\xa5");
	buf->cx = 0;
	buf->cy = 0;

	/* C-x r SPC a: save point in register a. */
	int name[] = { 'a' };
	pressWithKeys(CMD_POINT_REGISTER, name, 1);

	/* The preview, drawn while the prompt waits. */
	int quit[] = { KEY_CTRL('g') };
	pressWithKeys(CMD_VIEW_REGISTER, quit, 1);
	TEST_ASSERT(E.render_buf.len > 0);
	static const char want[] = "a^Jb\xe6\x97\xa5";
	int found = 0;
	for (int i = 0; i + 7 <= E.render_buf.len; i++)
		if (memcmp(E.render_buf.b + i, want, 7) == 0)
			found = 1;
	TEST_ASSERT(found);

	/* The output: a real newline would have been inside row 0. */
	pressWithKeys(CMD_VIEW_REGISTER, name, 1);
	struct buffer *out = shownBuffer("*Output*");
	TEST_ASSERT_NOT_NULL(out);
	if (out) {
		TEST_ASSERT(out->numrows >= 1);
		const char *row = (const char *)out->row[0].chars;
		TEST_ASSERT(memchr(row, '\n', out->row[0].size) == NULL);
		TEST_ASSERT(strstr(row, "buffer a^Jb\xe6\x97\xa5, line 1") !=
			    NULL);
	}

	freeMinibuffer();
	cleanupTestEditor();
}

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_register_preview_cuts_on_character_boundary);
	RUN_TEST(test_register_point_filename_newline);
	RUN_TEST(test_render_incomplete_sequence_at_row_end);
	RUN_TEST(test_cols_to_bytes_stops_at_span_end);
	return TEST_END();
}
