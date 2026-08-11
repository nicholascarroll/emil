/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_regex_semantics.c: the POSIX regex behaviour emil's search
 * design depends on, asserted against the platform's libc.
 *
 * Under the row array these properties are invisible: every regexec()
 * runs against a single row, which contains no newline, so REG_NEWLINE
 * changes nothing and its absence is harmless.  Once search runs over
 * a whole buffer, each of them becomes load-bearing:
 *
 *   - '.' and '[^x]' must not cross a newline, or a greedy quantifier
 *     runs to end of buffer instead of end of line.  This is also what
 *     Emacs does, so it is a compatibility requirement as well as a
 *     performance one.
 *   - '^' and '$' must anchor at line boundaries, not only at the ends
 *     of the subject.  Without REG_NEWLINE a whole-buffer search would
 *     match '^' at offset 0 alone.
 *   - a literal newline in the pattern -- what C-q C-n inserts -- must
 *     still match a newline in the subject.  REG_NEWLINE changes what
 *     the metacharacters do, not what a literal byte matches.  This is
 *     what lets multi-line patterns work in all four search modes.
 *
 * The last of those is the one worth stating explicitly: it is easy to
 * assume REG_NEWLINE makes newlines unmatchable altogether, which
 * would rule out multi-line search.  It does not.
 *
 * REG_NEWLINE itself needs no availability guard: POSIX.1-2001 lists
 * it as a regcomp() flag alongside REG_EXTENDED.  What is not
 * guaranteed is that every implementation agrees on the details, or
 * that any of them backtracks at a comparable speed -- hence this
 * suite rather than a compile-time check.
 */

#include "test.h"
#include "test_harness.h"
#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void setUp(void) {
}

void tearDown(void) {
}

/* Returns 1 on match, 0 on no match, -1 if the pattern did not
 * compile.  A compile failure is reported distinctly: it means the
 * platform rejected valid ERE, which is a different fault from a
 * semantic disagreement. */
static int tryMatch(const char *pat, const char *subject, int cflags) {
	regex_t re;
	regmatch_t m[1];
	if (regcomp(&re, pat, REG_EXTENDED | cflags) != 0)
		return -1;
	int rc = regexec(&re, subject, 1, m, 0);
	regfree(&re);
	return rc == 0 ? 1 : 0;
}

/* ---- Containment: what a match may not cross ---- */

/* '.' must stop at a newline.  If it does not, a pattern like "f.*z"
 * matches across the whole buffer and every greedy search becomes
 * quadratic in buffer length rather than in line length. */
void test_dot_does_not_cross_newline(void) {
	TEST_ASSERT_EQUAL_INT(0,
			      tryMatch("foo.bar", "foo\nbar", REG_NEWLINE));
}

void test_dot_star_does_not_cross_newline(void) {
	TEST_ASSERT_EQUAL_INT(0, tryMatch("f.*z", "foo\nbaz", REG_NEWLINE));
}

/* A non-matching list must also stop at a newline, for the same
 * reason: "[^x]+" is the other way to write "any character". */
void test_negated_class_does_not_cross_newline(void) {
	TEST_ASSERT_EQUAL_INT(0,
			      tryMatch("foo[^x]bar", "foo\nbar", REG_NEWLINE));
}

/* The containment property that chunking depends on, stated directly:
 * a pattern with no newline in it cannot match text spanning one, so
 * a line boundary is always a safe place to cut the subject. */
void test_newline_free_pattern_cannot_span_lines(void) {
	TEST_ASSERT_EQUAL_INT(0, tryMatch("ab", "a\nb", REG_NEWLINE));
	TEST_ASSERT_EQUAL_INT(0, tryMatch("a.b", "a\nb", REG_NEWLINE));
	TEST_ASSERT_EQUAL_INT(0, tryMatch("a[^q]b", "a\nb", REG_NEWLINE));
}

/* ---- Literal newlines: what C-q C-n must be able to express ---- */

void test_literal_newline_in_pattern_matches(void) {
	TEST_ASSERT_EQUAL_INT(1,
			      tryMatch("foo\nbar", "foo\nbar", REG_NEWLINE));
}

void test_literal_newline_with_metacharacters(void) {
	/* Each side of the newline may still use metacharacters; only
	 * crossing it requires the literal. */
	TEST_ASSERT_EQUAL_INT(1,
			      tryMatch("f.o\nb.r", "foo\nbar", REG_NEWLINE));
}

void test_two_literal_newlines_span_three_lines(void) {
	TEST_ASSERT_EQUAL_INT(
		1, tryMatch("a\nb\nc", "x\na\nb\nc\ny", REG_NEWLINE));
}

/* ---- Anchors ---- */

void test_caret_anchors_at_line_start(void) {
	TEST_ASSERT_EQUAL_INT(1, tryMatch("^bar", "foo\nbar", REG_NEWLINE));
}

void test_dollar_anchors_at_line_end(void) {
	TEST_ASSERT_EQUAL_INT(1, tryMatch("foo$", "foo\nbar", REG_NEWLINE));
}

/* Without REG_NEWLINE the anchors bind only to the ends of the whole
 * subject.  Asserted so the reason the flag is mandatory stays visible
 * in the suite rather than living only in a comment. */
void test_anchors_need_the_flag(void) {
	TEST_ASSERT_EQUAL_INT(0, tryMatch("^bar", "foo\nbar", 0));
	TEST_ASSERT_EQUAL_INT(0, tryMatch("foo$", "foo\nbar", 0));
}

/* ---- Greedy quantifiers stop at end of line ---- */

void test_greedy_run_bounded_by_line(void) {
	/* "o+" matches within the first line and does not reach across
	 * to consume anything after the newline. */
	regex_t re;
	regmatch_t m[1];
	TEST_ASSERT_EQUAL_INT(
		0, regcomp(&re, "o+", REG_EXTENDED | REG_NEWLINE));
	TEST_ASSERT_EQUAL_INT(0, regexec(&re, "fooo\nooo", 1, m, 0));
	TEST_ASSERT_EQUAL_INT(1, (int)m[0].rm_so);
	TEST_ASSERT_EQUAL_INT(4, (int)m[0].rm_eo);
	regfree(&re);
}

/* ---- Cancellation budget ----
 *
 * Search is cancellable because it runs in bounded chunks and polls
 * between them, so the worst-case time for one chunk is the worst-case
 * cancel latency.  That bound is a property of the platform's regex
 * engine, not of emil: a backtracking engine that does not confine a
 * greedy run to one line spends quadratic time in the chunk instead of
 * in the line, and C-g stops being responsive.
 *
 * The threshold is deliberately loose.  This is not a benchmark; it is
 * a check that the engine is in the right complexity class, and it
 * should not fail on a loaded or slow machine.  A platform that fails
 * it needs a smaller chunk, not a different flag.
 */
#define CHUNK_BYTES (64 * 1024)
#define BUDGET_MS 250.0

static double elapsedMs(struct timespec a, struct timespec b) {
	return (double)(b.tv_sec - a.tv_sec) * 1000.0 +
	       (double)(b.tv_nsec - a.tv_nsec) / 1000000.0;
}

/* Time one chunk-sized regexec with a pattern whose greedy head would
 * run to end of subject if newlines did not stop it, against a subject
 * with no match in it -- the worst case, since the engine must try
 * every starting position before giving up. */
static double timeWorstCaseChunk(int cflags) {
	char *p = malloc(CHUNK_BYTES + 1);
	for (size_t i = 0; i < CHUNK_BYTES; i++)
		p[i] = (i % 80 == 79) ? '\n' : (char)('a' + (i % 23));
	p[CHUNK_BYTES] = '\0';

	regex_t re;
	regmatch_t m[1];
	if (regcomp(&re, ".+NEEDLE", REG_EXTENDED | cflags) != 0) {
		free(p);
		return -1.0;
	}
	struct timespec t0, t1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	regexec(&re, p, 1, m, 0);
	clock_gettime(CLOCK_MONOTONIC, &t1);
	regfree(&re);
	free(p);
	return elapsedMs(t0, t1);
}

void test_chunk_stays_within_cancel_budget(void) {
	double ms = timeWorstCaseChunk(REG_NEWLINE);
	if (ms > BUDGET_MS) {
		printf("    64 KiB chunk took %.1f ms, budget %.1f ms -- "
		       "reduce the search chunk size on this platform\n",
		       ms, BUDGET_MS);
	}
	TEST_ASSERT_TRUE(ms >= 0.0 && ms <= BUDGET_MS);
}

int main(void) {
	TEST_BEGIN();

	/* Containment */
	RUN_TEST(test_dot_does_not_cross_newline);
	RUN_TEST(test_dot_star_does_not_cross_newline);
	RUN_TEST(test_negated_class_does_not_cross_newline);
	RUN_TEST(test_newline_free_pattern_cannot_span_lines);

	/* Literal newlines (C-q C-n) */
	RUN_TEST(test_literal_newline_in_pattern_matches);
	RUN_TEST(test_literal_newline_with_metacharacters);
	RUN_TEST(test_two_literal_newlines_span_three_lines);

	/* Anchors */
	RUN_TEST(test_caret_anchors_at_line_start);
	RUN_TEST(test_dollar_anchors_at_line_end);
	RUN_TEST(test_anchors_need_the_flag);

	/* Greedy quantifiers */
	RUN_TEST(test_greedy_run_bounded_by_line);

	/* Cancellation budget */
	RUN_TEST(test_chunk_stays_within_cancel_budget);

	return TEST_END();
}
