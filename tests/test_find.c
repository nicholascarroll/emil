/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_find.c: Incremental search: direction, repeat, wrapping. */

#include "test.h"
#include "test_harness.h"
#include "find.h"
#include "prompt.h"
#include "keymap.h"
#include "edit.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ---- B5: reverse search must not move forward past point.
 *
 * The "match on the same row as the cursor" block was hardcoded
 * forward-only (strstr from cx + 1) and never consulted direction, so
 * C-r found the next match after point instead of the previous one. */

void test_reverse_search_goes_backward(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "aaa", "foo bar foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 5; /* between the 'f' at col 0 and the one at col 8 */

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	reverseFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx); /* was 9: forward past point */
	TEST_ASSERT(E.buf->cx < 5);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Stepping to an earlier row backwards should land on that row's LAST
 * match, otherwise repeated C-r skips every match but the first: the
 * same-row block finds nothing before the first match and steps back
 * another row. */
void test_reverse_search_previous_row_takes_last_match(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "foo bar foo", "aaa" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 0;

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	reverseFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(8, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Forward search on the cursor's own row starts at the cursor column.
 *
 * This test previously asserted cx == 0 and carried a comment saying
 * that a forward search scans from the top of the buffer rather than
 * from point, pinning that as intended behaviour.  It was not
 * intended: it is B14, and the assertion is now the Emacs one.  With
 * the cursor at column 5 of "foo bar foo", the match before point at
 * column 0 must be skipped in favour of the one at column 8. */
void test_forward_search_first_char_from_point(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "aaa", "foo bar foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 5;

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(8, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* C-s repeat drives the forward same-row block, which is the branch
 * the reverse fix sits next to.  The cursor starts at column 0 so that
 * the fresh search matches at point (column 0) and the repeat then has
 * somewhere to advance to (column 8); starting at column 5 would make
 * the fresh search land on column 8 already and the repeat would
 * wrap, which is a different code path. */
void test_forward_search_repeat_advances(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "aaa", "foo bar foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 0;

	int keys[] = { 'f', CTRL('s'), '\r' };
	scriptKeys(keys, 3);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(8, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* ---- B14: forward search must start from point, not the top of the
 * buffer.
 *
 * Reported against the released build: C-s always began scanning at
 * row 0.  The seeding line read
 *
 *     current = (direction == -1) ? bufr->cy : -1;
 *
 * so a fresh backward search started on the cursor's row but a fresh
 * forward one started at -1, and the row-stepping loop below then
 * began at row 0.  Emacs searches forward from point and only wraps
 * to the top after passing the end of the buffer. */

void test_forward_search_starts_from_point(void) {
	initTestEditor();
	makeMinibuffer();
	/* "foo" appears both above and below the cursor. */
	const char *lines[] = { "foo above", "bar", "foo below" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->cy = 1;
	buf->cx = 0;

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	/* Must find the occurrence *below* point, not the one above. */
	TEST_ASSERT_EQUAL_INT(2, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* A match beginning exactly at point is a valid first match: Emacs
 * C-s finds the occurrence under the cursor rather than skipping it. */
void test_forward_search_matches_at_point(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "foo above", "foo at point" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 0;

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Within a row, a fresh forward search starts at the cursor column. */
void test_forward_search_starts_from_column(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *buf = make_test_buffer("foo bar foo");
	buf->cy = 0;
	buf->cx = 5;

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(8, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* A forward search that finds nothing between point and the end of the
 * buffer FAILS THERE.  It does not quietly continue round the top and
 * land on a match above the starting point -- doing that moves point
 * backwards, which is what C-s must never do on its own.
 *
 * This test previously asserted the opposite, on the mistaken view
 * that wrapping was part of searching from point.  Emacs reports
 * "Failing I-search" and leaves point alone; only a further C-s wraps,
 * which is the next test. */
void test_forward_search_does_not_wrap_on_first_pass(void) {
	initTestEditor();
	makeMinibuffer();
	/* "target" exists only ABOVE the cursor. */
	const char *lines[] = { "target here", "zzz", "zzz" };
	struct buffer *lbuf = make_test_buffer_lines(lines, 3);
	lbuf->cy = 1;
	lbuf->cx = 0;

	int keys[] = { 't', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	/* Point unmoved, and nothing highlighted as a match. */
	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);
	TEST_ASSERT_EQUAL_INT(0, E.buf->match);

	freeMinibuffer();
	cleanupTestEditor();
}

/* A second C-s after a failing search is Emacs's wrap gesture: now the
 * occurrence above point is reachable. */
void test_forward_search_repeat_wraps_after_failing(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "target here", "zzz", "zzz" };
	struct buffer *lbuf = make_test_buffer_lines(lines, 3);
	lbuf->cy = 1;
	lbuf->cx = 0;

	int keys[] = { 't', CTRL('s'), '\r' };
	scriptKeys(keys, 3);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Backward search is symmetric: nothing above point means the pass
 * fails rather than continuing round the bottom. */
void test_reverse_search_does_not_wrap_on_first_pass(void) {
	initTestEditor();
	makeMinibuffer();
	/* "target" exists only BELOW the cursor. */
	const char *lines[] = { "zzz", "zzz", "target here" };
	struct buffer *lbuf = make_test_buffer_lines(lines, 3);
	lbuf->cy = 1;
	lbuf->cx = 0;

	int keys[] = { 't', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	reverseFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);
	TEST_ASSERT_EQUAL_INT(0, E.buf->match);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Deleting a character in the minibuffer re-runs the search from the
 * origin, not from wherever the longer query had landed the cursor.
 * "a" first occurs at column 1, "ab" only at column 4; after
 * backspacing the search must return to column 1. */
void test_forward_search_backspace_returns_to_origin(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *buf = make_test_buffer("xa yab");
	buf->cy = 0;
	buf->cx = 0;

	int keys[] = { 'a', 'b', KEY_BACKSPACE, '\r' };
	scriptKeys(keys, 4);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(1, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* These tests manage the editor themselves. */
void setUp(void) {
}

void tearDown(void) {
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_reverse_search_goes_backward);
	RUN_TEST(test_reverse_search_previous_row_takes_last_match);
	RUN_TEST(test_forward_search_first_char_from_point);
	RUN_TEST(test_forward_search_repeat_advances);
	RUN_TEST(test_forward_search_starts_from_point);
	RUN_TEST(test_forward_search_matches_at_point);
	RUN_TEST(test_forward_search_starts_from_column);
	RUN_TEST(test_forward_search_does_not_wrap_on_first_pass);
	RUN_TEST(test_forward_search_repeat_wraps_after_failing);
	RUN_TEST(test_reverse_search_does_not_wrap_on_first_pass);
	RUN_TEST(test_forward_search_backspace_returns_to_origin);

	return TEST_END();
}
