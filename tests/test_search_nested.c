/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_search_nested.c: C-M-s / C-M-r inside a search repeat it, as in
 * Emacs, and no second search can start inside a running one.
 *
 * find.c keeps the interactive search's origin and mode in file-scope
 * state that searchInteractive() writes before it opens its prompt.
 * The prompt used to dispatch C-M-s / C-M-r as ordinary commands, so
 * pressing either inside a search started a second searchInteractive.
 * editorPrompt refused the nested prompt, but only after that shared
 * state had been overwritten with the minibuffer's cursor, and the
 * nested call's cleanup cleared regex_mode.  The outer search then
 * dropped to a literal search from the wrong origin; with a multi-row
 * minibuffer the origin row was past the end of the searched buffer
 * and findCallback read its row array out of bounds.
 *
 * Now the prompt treats C-M-s / C-M-r as C-s / C-r in a search
 * (isearch-repeat-forward / -backward), and searchInteractive refuses
 * to start inside a prompt before it touches any state, which covers
 * the routes that bypass the prompt's key handling, such as a keyboard
 * macro run from inside it.  Found by the pre-1.0 keystroke fuzzer
 * (tests/fuzz_keys.c). */

#include "test.h"
#include "test_harness.h"
#include "find.h"
#include "region.h"
#include "keymap.h"
#include <string.h>

/* C-M-s repeats a forward regexp search, and it stays a regexp search. */
void test_cms_repeats_regex_search(void) {
	initTestEditor();
	makeMinibuffer();

	const char *lines[] = { "foo1 xx", "bar", "foo2" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int keys[] = { 'f', 'o', 'o',
		       '[', '0', '-',
		       '9', ']', KEY_META(CTRL('s')),
		       '\r' };
	scriptKeys(keys, 10);
	muteStdout();
	regexFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(2, buf->cy);
	TEST_ASSERT_EQUAL_INT(0, buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* C-M-r repeats a backward regexp search. */
void test_cmr_repeats_backward_regex_search(void) {
	initTestEditor();
	makeMinibuffer();

	const char *lines[] = { "foo1 xx", "bar", "foo2" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->cy = 2;
	buf->cx = 4;

	int keys[] = { 'f', 'o', 'o',
		       '[', '0', '-',
		       '9', ']', KEY_META(CTRL('r')),
		       '\r' };
	scriptKeys(keys, 10);
	muteStdout();
	backwardRegexFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT_EQUAL_INT(0, buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* The fuzzer's sequence: a multi-row minibuffer, then C-M-s.  It used
 * to leave the search origin on a minibuffer row past the end of the
 * one-row buffer being searched. */
void test_cms_with_multirow_minibuffer(void) {
	initTestEditor();
	makeMinibuffer();

	const char *lines[] = { "alpha beta", "gamma delta", "epsilon",
				"zeta eta" };
	struct buffer *buf = make_test_buffer_lines(lines, 4);

	/* C-x h C-w: the buffer is left as one empty row and the
	 * four-line text is on the kill ring. */
	markBuffer();
	killRegion();
	TEST_ASSERT(bufferIsEmpty(buf));

	/* C-y grows the minibuffer to five rows; C-M-s; C-g. */
	int keys[] = { CTRL('y'), KEY_META(CTRL('s')), CTRL('g') };
	scriptKeys(keys, 3);
	muteStdout();
	backwardRegexFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);

	freeMinibuffer();
	cleanupTestEditor();
}

/* A keyboard macro run from inside a search reaches the search
 * commands through processKeypress, not through the prompt's keys.
 * The inner search must be refused without disturbing the outer one:
 * "foo" typed from (0,0) must still be found at (0,0). */
void test_macro_search_inside_search_is_refused(void) {
	initTestEditor();
	makeMinibuffer();

	const char *lines[] = { "foo zero", "one", "foo two" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	E.macro.keys = xmalloc(sizeof(int));
	E.macro.keys[0] = KEY_META(CTRL('s'));
	E.macro.nkeys = 1;
	E.macro.skeys = 1;

	/* foo, then C-x e (run the macro), then RET. */
	int keys[] = { 'f', 'o', 'o', CTRL('x'), 'e', '\r' };
	scriptKeys(keys, 6);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT_EQUAL_INT(0, buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

void setUp(void) {
}
void tearDown(void) {
}

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_cms_repeats_regex_search);
	RUN_TEST(test_cmr_repeats_backward_regex_search);
	RUN_TEST(test_cms_with_multirow_minibuffer);
	RUN_TEST(test_macro_search_inside_search_is_refused);
	return TEST_END();
}
