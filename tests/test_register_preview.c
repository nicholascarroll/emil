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
#include "register.h"
#include "keymap.h"
#include <string.h>

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
	const char *lines[] = { "text" };
	make_test_buffer_lines(lines, 1);

	E.registers['a'].rtype = REGISTER_TEXT;
	E.registers['a'].data.text = (struct text){ 0 };
	E.registers['a'].data.text.str = (uint8_t *)cjkText();

	/* C-x r v, then C-g at the register prompt.  The last frame
	 * drawn is the one showing the preview. */
	int keys[] = { CTRL('g') };
	scriptKeys(keys, 1);
	muteStdout();
	viewRegister();
	unmuteStdout();
	clearKeys();

	/* Before the fix the frame carried the row's NUL terminator
	 * (and a byte past it) after the dangling lead byte. */
	TEST_ASSERT(E.render_buf.len > 0);
	TEST_ASSERT(memchr(E.render_buf.b, '\0', E.render_buf.len) == NULL);
	TEST_ASSERT(utf8_validate((const uint8_t *)E.render_buf.b,
				  E.render_buf.len));

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

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_register_preview_cuts_on_character_boundary);
	RUN_TEST(test_render_incomplete_sequence_at_row_end);
	RUN_TEST(test_cols_to_bytes_stops_at_span_end);
	return TEST_END();
}
