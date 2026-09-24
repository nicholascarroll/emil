/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_search_stale.c: a new incremental search must not resume from
 * the row where the previous one stopped.
 *
 * findCallback kept the row of its last match in a function static,
 * last_match, and reset it only when called with C-g, C-c or RET.  It
 * never was: editorPrompt handles those three keys itself and leaves
 * before the callback runs.  So the row outlived the search that set
 * it, and a new search begun with C-s C-s (which recalls the previous
 * pattern) looked like a repeat and resumed from it.
 *
 * In the same buffer that searches from the wrong place.  In a buffer
 * with fewer rows -- the old one after an edit, or simply another
 * buffer -- the row-stepping loop, which tested only for equality with
 * -1 and numrows, stepped further past the end and indexed the row
 * array out of bounds: a heap-use-after-free under `make sanitize`,
 * and a segfault in a release build after C-x b to a smaller buffer.
 *
 * The row now lives with the rest of the search state and
 * searchInteractive resets it for every search; the loop tests the
 * range.  Found by the pre-1.0 keystroke fuzzer (tests/fuzz_keys.c). */

#include "test.h"
#include "test_harness.h"
#include "find.h"
#include "region.h"
#include "keymap.h"
#include <string.h>

/* C-s C-s searches from point, not from where the last search ended.
 * Fails deterministically in a plain build without the fix. */
void test_repeat_search_starts_from_point(void) {
	initTestEditor();
	makeMinibuffer();

	const char *lines[] = { "a", "foo 1", "b", "c", "d", "e", "f", "foo 7" };
	struct buffer *buf = make_test_buffer_lines(lines, 8);

	/* foo, C-s, RET: the second match, on row 7. */
	int first[] = { 'f', 'o', 'o', CTRL('s'), '\r' };
	scriptKeys(first, 5);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();
	TEST_ASSERT_EQUAL_INT(7, buf->cy);

	buf->cy = 0;
	buf->cx = 0;

	/* C-s on the empty prompt recalls "foo"; the next one after
	 * point is on row 1. */
	int second[] = { CTRL('s'), '\r' };
	scriptKeys(second, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();
	TEST_ASSERT_EQUAL_INT(1, buf->cy);
	TEST_ASSERT_EQUAL_INT(0, buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* The same buffer, shrunk since the last search. */
void test_new_search_does_not_resume_stale_row(void) {
	initTestEditor();
	makeMinibuffer();

	const char *lines[] = { "one",	"two", "three", "four",
				"five", "six", "seven", "eight foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 8);

	int first[] = { 'f', 'o', 'o', '\r' };
	scriptKeys(first, 4);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();
	TEST_ASSERT_EQUAL_INT(7, buf->cy);

	/* C-x h, C-w: the buffer shrinks to a single empty row. */
	markBuffer();
	killRegion();
	TEST_ASSERT(bufferIsEmpty(buf));

	int second[] = { CTRL('s'), CTRL('g') };
	scriptKeys(second, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT(buf->numrows >= 1);

	freeMinibuffer();
	cleanupTestEditor();
}

/* A different, smaller buffer: the ordinary-use crash.  Search in a
 * long buffer, switch to a short one, C-s C-s. */
void test_new_search_in_smaller_buffer(void) {
	initTestEditor();
	makeMinibuffer();

	const char *biglines[40];
	for (int i = 0; i < 40; i++)
		biglines[i] = "line";
	biglines[38] = "line foo";
	struct buffer *big = make_test_buffer_lines(biglines, 40);

	int first[] = { 'f', 'o', 'o', '\r' };
	scriptKeys(first, 4);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();
	TEST_ASSERT_EQUAL_INT(38, big->cy);

	const char *smalllines[] = { "one", "two" };
	struct buffer *small = make_test_buffer_lines(smalllines, 2);
	small->next = big; /* keep both in the list for cleanup */

	int second[] = { CTRL('s'), CTRL('g') };
	scriptKeys(second, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT(E.buf == small);
	TEST_ASSERT_EQUAL_INT(0, small->cy);

	freeMinibuffer();
	cleanupTestEditor();
}

void setUp(void) {
}
void tearDown(void) {
}

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_repeat_search_starts_from_point);
	RUN_TEST(test_new_search_does_not_resume_stale_row);
	RUN_TEST(test_new_search_in_smaller_buffer);
	return TEST_END();
}
