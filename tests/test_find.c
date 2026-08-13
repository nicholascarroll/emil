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

/* B5: reverse search must not move forward past point.
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

/* B14: forward search must start from point, not the top of the
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

/* A1: `^` must not match at a mid-row restart.
 *
 * POSIX treats the first byte of the subject as a line beginning
 * unless REG_NOTBOL is passed, and the forward-repeat path searches
 * &row->chars[start].  With point past column 0 an anchored pattern
 * therefore matched at whatever offset `start` happened to be.
 * Reproduced directly against glibc 2.39: subject "xfoo bar" from
 * column 1 matches `^foo`; with REG_NOTBOL it does not.
 *
 * Point starts at column 1 of "xfoo", i.e. on the 'f', which is NOT a
 * line beginning.  The only true `^foo` in the buffer is on the row
 * below, so the two behaviours land point in different places rather
 * than merely succeeding or failing: unfixed lands on (0,1), fixed on
 * (1,0).  A negative assertion would have been muddied by incremental
 * search moving point as each character of the pattern is typed --
 * `^` on its own matches the start of any row. */
void test_regex_bol_does_not_match_mid_row(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "xfoo", "foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 0;
	buf->cx = 1;

	int keys[] = { '^', 'f', 'o', 'o', '\r' };
	scriptKeys(keys, 5);
	muteStdout();
	regexFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy); /* was 0: matched inside xfoo */
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx); /* was 1 */

	freeMinibuffer();
	cleanupTestEditor();
}

/* The other half of A1: REG_NOTBOL must NOT be passed when the subject
 * really does begin at byte 0, or `^` would stop working entirely.
 *
 * Point starts on row 0, so a working `^foo` has to move it to row 1.
 * An earlier version of this test put point at (0,0) on a buffer whose
 * only row was "foo" and asserted point stayed at (0,0) -- which a
 * search that found nothing at all also satisfies.  It passed with
 * REG_NOTBOL applied unconditionally, i.e. it was testing nothing.
 * Requiring the match to move point is what makes the failure
 * distinguishable from the search failing. */
void test_regex_bol_matches_at_row_start(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "zzz", "foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 0;
	buf->cx = 0;

	int keys[] = { '^', 'f', 'o', 'o', '\r' };
	scriptKeys(keys, 5);
	muteStdout();
	regexFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* An unanchored pattern must be unaffected by the new flag: REG_NOTBOL
 * governs `^` only.  Point at column 1, so the match at 0 is behind us
 * and the one at 8 is the answer.
 *
 * Characterisation, not regression: this is asserted because §A1 asks
 * for it, but no variant of this change makes it fail -- REG_NOTBOL
 * cannot affect a pattern with no anchor.  Recorded as such rather
 * than left looking like a test that has been shown to bite. */
void test_regex_unanchored_unaffected_by_notbol(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *buf = make_test_buffer("foo bar foo");
	buf->cy = 0;
	buf->cx = 1;

	int keys[] = { 'f', 'o', 'o', '\r' };
	scriptKeys(keys, 4);
	muteStdout();
	regexFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(8, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* A2: searchRowBackward amplifies A1 into a consistently wrong result.
 *
 * The loop restarts from match + 1 and keeps the last match before its
 * limit.  With `^` matching at every restart position (A1), `best`
 * walked forward to whatever offset sat just under the limit rather
 * than staying on the real match.
 *
 * The pattern matters.  §A2 of the findings proposes `^foo` against
 * "foo x foo", but that does not discriminate: after the match at 0
 * the loop restarts at byte 1, where the subject is "oo x foo", `^foo`
 * fails immediately and the loop breaks with best still 0.  The
 * unfixed code returns the right answer there by accident.  The
 * amplification needs a pattern that matches at *consecutive* restart
 * positions, so `best` can walk.  `^a` against "aaa" does: unfixed it
 * matches at 0, then 1, then 2, and returns 2; fixed, only the restart
 * at byte 0 is a line beginning and it returns 0.
 *
 * No separate fix -- this falls out of passing REG_NOTBOL for
 * p > row->chars. */
void test_regex_backward_bol_finds_real_match(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *buf = make_test_buffer("aaa");
	buf->cy = 0;
	buf->cx = 3; /* end of row: search the whole row backwards */

	int keys[] = { '^', 'a', '\r' };
	scriptKeys(keys, 3);
	muteStdout();
	backwardRegexFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx); /* was 2: last restart under limit */

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
	RUN_TEST(test_forward_search_repeat_advances);
	RUN_TEST(test_forward_search_starts_from_point);
	RUN_TEST(test_forward_search_matches_at_point);
	RUN_TEST(test_forward_search_starts_from_column);
	RUN_TEST(test_forward_search_does_not_wrap_on_first_pass);
	RUN_TEST(test_forward_search_repeat_wraps_after_failing);
	RUN_TEST(test_reverse_search_does_not_wrap_on_first_pass);
	RUN_TEST(test_forward_search_backspace_returns_to_origin);

	/* A1/A2: regex anchoring at mid-row restarts */
	RUN_TEST(test_regex_bol_does_not_match_mid_row);
	RUN_TEST(test_regex_bol_matches_at_row_start);
	RUN_TEST(test_regex_unanchored_unaffected_by_notbol);
	RUN_TEST(test_regex_backward_bol_finds_real_match);

	return TEST_END();
}
