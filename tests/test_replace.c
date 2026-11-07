/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_replace.c: the replace-regexp substitution engine.
 *
 * Covers the two pure functions behind replaceRegex:
 * replacementTemplateError (template validation) and
 * regexSubstituteAll (matching and expansion).  Driving the pure
 * functions rather than replaceRegex keeps the prompts out of the way.
 *
 * Patterns containing a literal newline are reachable from the UI via
 * C-q C-j, so the cross-line cases below are live behaviour, not
 * hypotheticals. */

#include "test.h"
#include "test_harness.h"
#include "dbuf.h"
#include "keymap.h"
#include "mutate.h"
#include "region.h"
#include "undo.h"
#include <regex.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Run a substitution and return the rewritten span as a C string.
 * Sets *count, *first and *last to the engine's out-params. */
static char *sub(const char *pat, const char *subject, const char *tmpl,
		 int notbol, int noteol, int *count, int *first, int *last) {
	regex_t re;
	TEST_ASSERT_EQUAL_INT(
		0, regcomp(&re, pat, REG_EXTENDED | REG_NEWLINE));

	struct dbuf d = DBUF_INIT;
	int f = -1, l = -1;
	int n = regexSubstituteAll(&re, (const uint8_t *)subject,
				   (int)strlen(subject),
				   (const uint8_t *)tmpl, notbol, noteol, &d,
				   &f, &l);
	int out_len;
	uint8_t *out = dbuf_detach(&d, &out_len);
	regfree(&re);

	if (count)
		*count = n;
	if (first)
		*first = f;
	if (last)
		*last = l;
	return (char *)out;
}

/* Convenience for the common case: no anchoring restrictions. */
static char *sub_all(const char *pat, const char *subject, const char *tmpl,
		     int *count) {
	return sub(pat, subject, tmpl, 0, 0, count, NULL, NULL);
}

/* ---- All matches, not just the first per line ---- */

void test_replaces_every_match_on_a_line(void) {
	int n;
	char *r = sub_all("a", "banana", "X", &n);
	TEST_ASSERT_EQUAL_INT(3, n);
	TEST_ASSERT_EQUAL_STRING("XnXnX", r);
	free(r);
}

void test_replaces_across_multiple_lines(void) {
	int n;
	char *r = sub_all("beta", "alpha beta\nbeta gamma beta", "X", &n);
	TEST_ASSERT_EQUAL_INT(3, n);
	TEST_ASSERT_EQUAL_STRING("X\nX gamma X", r);
	free(r);
}

void test_no_match_leaves_count_zero(void) {
	int n;
	char *r = sub_all("zzz", "alpha beta", "X", &n);
	TEST_ASSERT_EQUAL_INT(0, n);
	free(r);
}

/* ---- Rewritten span is narrowed to first..last match ---- */

void test_span_excludes_untouched_head_and_tail(void) {
	int n, first, last;
	char *r = sub("b+", "aaabbbccc", "X", 0, 0, &n, &first, &last);
	TEST_ASSERT_EQUAL_INT(1, n);
	/* Only "bbb" is rewritten; "aaa" and "ccc" are left alone. */
	TEST_ASSERT_EQUAL_INT(3, first);
	TEST_ASSERT_EQUAL_INT(6, last);
	TEST_ASSERT_EQUAL_STRING("X", r);
	free(r);
}

void test_span_covers_gap_between_matches(void) {
	int n, first, last;
	char *r = sub("b", "abcb d", "X", 0, 0, &n, &first, &last);
	TEST_ASSERT_EQUAL_INT(2, n);
	TEST_ASSERT_EQUAL_INT(1, first);
	TEST_ASSERT_EQUAL_INT(4, last);
	TEST_ASSERT_EQUAL_STRING("XcX", r);
	free(r);
}

/* ---- Capture groups and \& ---- */

void test_whole_match_reference(void) {
	int n;
	char *r = sub_all("[0-9]+", "a12b", "<\\&>", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("<12>", r);
	free(r);
}

void test_numbered_groups(void) {
	int n;
	char *r = sub_all("([a-z]+)=([0-9]+)", "key=42", "\\2:\\1", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("42:key", r);
	free(r);
}

void test_group_swap_repeats_per_match(void) {
	int n;
	char *r = sub_all("([a-z])([0-9])", "a1 b2", "\\2\\1", &n);
	TEST_ASSERT_EQUAL_INT(2, n);
	TEST_ASSERT_EQUAL_STRING("1a 2b", r);
	free(r);
}

void test_nonparticipating_group_expands_empty(void) {
	int n;
	/* The (x) alternative never participates, so \2 is empty. */
	char *r = sub_all("(a)|(x)", "a", "[\\1\\2]", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("[a]", r);
	free(r);
}

void test_escaped_backslash_is_literal(void) {
	int n;
	char *r = sub_all("a", "a", "\\\\", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("\\", r);
	free(r);
}

/* ---- Template validation ---- */

void test_valid_templates_accepted(void) {
	TEST_ASSERT_NULL(replacementTemplateError((const uint8_t *)"plain", 0));
	TEST_ASSERT_NULL(replacementTemplateError((const uint8_t *)"<\\&>", 0));
	TEST_ASSERT_NULL(replacementTemplateError((const uint8_t *)"\\\\", 0));
	TEST_ASSERT_NULL(
		replacementTemplateError((const uint8_t *)"\\1-\\2", 2));
}

void test_unknown_escape_rejected(void) {
	/* \n and \t are errors, not newline and tab: see region.c. */
	TEST_ASSERT_NOT_NULL(
		replacementTemplateError((const uint8_t *)"a\\nb", 0));
	TEST_ASSERT_NOT_NULL(
		replacementTemplateError((const uint8_t *)"a\\tb", 0));
	TEST_ASSERT_NOT_NULL(
		replacementTemplateError((const uint8_t *)"\\q", 0));
	/* \0 is not accepted as a synonym for \&. */
	TEST_ASSERT_NOT_NULL(
		replacementTemplateError((const uint8_t *)"\\0", 1));
}

void test_trailing_backslash_rejected(void) {
	TEST_ASSERT_NOT_NULL(
		replacementTemplateError((const uint8_t *)"abc\\", 0));
}

void test_out_of_range_group_rejected(void) {
	TEST_ASSERT_NOT_NULL(
		replacementTemplateError((const uint8_t *)"\\3", 2));
	TEST_ASSERT_NOT_NULL(
		replacementTemplateError((const uint8_t *)"\\1", 0));
	TEST_ASSERT_NULL(replacementTemplateError((const uint8_t *)"\\2", 2));
}

/* ---- Anchoring ---- */

void test_caret_anchors_at_each_line(void) {
	int n, first, last;
	char *r = sub("^a", "abc\nabc", "X", 0, 0, &n, &first, &last);
	TEST_ASSERT_EQUAL_INT(2, n);
	/* Span runs from the first match to the last, so the trailing
	 * "bc" after the final match is not part of the output. */
	TEST_ASSERT_EQUAL_INT(0, first);
	TEST_ASSERT_EQUAL_INT(5, last);
	TEST_ASSERT_EQUAL_STRING("Xbc\nX", r);
	free(r);
}

void test_dollar_anchors_at_each_line(void) {
	int n, first, last;
	char *r = sub("c$", "abc\nabc", "X", 0, 0, &n, &first, &last);
	TEST_ASSERT_EQUAL_INT(2, n);
	/* Leading "ab" precedes the first match, so it is outside the
	 * span too. */
	TEST_ASSERT_EQUAL_INT(2, first);
	TEST_ASSERT_EQUAL_INT(7, last);
	TEST_ASSERT_EQUAL_STRING("X\nabX", r);
	free(r);
}

void test_notbol_suppresses_caret_at_subject_start(void) {
	int n;
	/* Region starting mid-line: ^ must not match its first byte,
	 * but must still match after the embedded newline. */
	char *r = sub("^a", "abc\nabc", "X", 1, 0, &n, NULL, NULL);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("X", r);
	free(r);
}

void test_noteol_suppresses_dollar_at_subject_end(void) {
	int n;
	char *r = sub("c$", "abc\nabc", "X", 0, 1, &n, NULL, NULL);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("X", r);
	free(r);
}

void test_dot_does_not_cross_newline(void) {
	int n;
	char *r = sub_all("a.c", "a\nc", "X", &n);
	TEST_ASSERT_EQUAL_INT(0, n);
	free(r);
}

/* Patterns containing a literal newline.  Reachable since C-q C-j:
 * these are the portability surface, because REG_NEWLINE is where
 * libc implementations could plausibly differ.  Verified identical on
 * glibc and musl; run the suite on macOS, MSYS2 and bionic to confirm
 * the POSIX guarantee holds there too. */
void test_literal_newline_in_pattern_matches(void) {
	int n;
	char *r = sub_all("b\nc", "ab\ncd", "X", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("X", r);
	free(r);
}

void test_newline_in_bracket_expression(void) {
	int n;
	char *r = sub_all("b[\n]c", "ab\ncd", "X", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("X", r);
	free(r);
}

/* Under REG_NEWLINE a negated class must NOT match a newline, which is
 * what stops a replace running away across lines. */
void test_negated_class_does_not_match_newline(void) {
	int n;
	char *r = sub_all("b[^x]c", "ab\ncd", "X", &n);
	TEST_ASSERT_EQUAL_INT(0, n);
	free(r);
}

void test_blank_line_pattern(void) {
	int n;
	char *r = sub_all("\n\n", "a\n\nb", "X", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	/* sub_all returns the rewritten span, not the whole subject. */
	TEST_ASSERT_EQUAL_STRING("X", r);
	free(r);
}

void test_newline_under_repetition(void) {
	int n;
	char *r = sub_all("a\n+b", "a\n\n\nb", "X", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("X", r);
	free(r);
}

/* ---- Zero-width matches ---- */

void test_zero_width_match_terminates(void) {
	int n;
	/* x* matches empty at every position; without an explicit
	 * step this would spin forever. */
	char *r = sub_all("x*", "ab", "-", &n);
	TEST_ASSERT_EQUAL_INT(3, n);
	TEST_ASSERT_EQUAL_STRING("-a-b-", r);
	free(r);
}

void test_zero_width_mixed_with_real_match(void) {
	int n;
	/* The empty match sitting immediately after the "a" must not
	 * count as its own occurrence, or this yields -b--b-. */
	char *r = sub_all("a*", "bab", "-", &n);
	TEST_ASSERT_EQUAL_INT(3, n);
	TEST_ASSERT_EQUAL_STRING("-b-b-", r);
	free(r);
}

void test_zero_width_step_preserves_utf8(void) {
	int n;
	/* Stepping one byte instead of one character would split the
	 * two-byte é and corrupt the buffer. */
	char *r = sub_all("x*", "caf\xc3\xa9", "", &n);
	TEST_ASSERT_EQUAL_INT(5, n);
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", r);
	free(r);
}

void test_end_anchor_zero_width_terminates(void) {
	int n;
	char *r = sub_all("$", "ab", "!", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("!", r);
	free(r);
}

/* ---- Multibyte payloads ---- */

void test_replacement_may_contain_utf8(void) {
	int n;
	char *r = sub_all("cafe", "cafe", "caf\xc3\xa9", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", r);
	free(r);
}

void test_match_may_contain_utf8(void) {
	int n;
	char *r = sub_all("caf\xc3\xa9", "a caf\xc3\xa9 b", "<\\&>", &n);
	TEST_ASSERT_EQUAL_INT(1, n);
	TEST_ASSERT_EQUAL_STRING("<caf\xc3\xa9>", r);
	free(r);
}

/* ---- Runner ---- */

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- replaceRegex end to end, with its history ---- */

/* The text as one string, rows joined with newlines. */
static char *buffer_text(const struct buffer *buf) {
	struct dbuf d = DBUF_INIT;
	for (int i = 0; i < buf->numrows; i++) {
		if (i > 0)
			dbuf_append(&d, (const uint8_t *)"\n", 1);
		dbuf_append(&d, buf->row[i].chars, buf->row[i].size);
	}
	return (char *)dbuf_detach(&d, NULL);
}

/* The record at the head must describe the text as it stands now. */
static void assert_record_matches(struct buffer *buf, const struct undo *u,
				  int must_be_present) {
	int ex, ey;
	computeInsertEnd(u->data, u->datalen, u->startx, u->starty, &ex, &ey);
	TEST_ASSERT_EQUAL_INT(ex, u->endx);
	TEST_ASSERT_EQUAL_INT(ey, u->endy);
	TEST_ASSERT_TRUE(bufferPosValid(buf, u->startx, u->starty));
	if (must_be_present) {
		int n;
		TEST_ASSERT_TRUE(bufferRangeValid(buf, u->startx, u->starty,
						  u->endx, u->endy));
		uint8_t *t = collectRegionText(buf, u->startx, u->starty,
					       u->endx, u->endy, &n);
		TEST_ASSERT_EQUAL_INT(u->datalen, n);
		TEST_ASSERT_TRUE(memcmp(t, u->data, n) == 0);
		free(t);
	}
}

/* Run replaceRegex, then walk the whole history back and forward,
 * checking every record against the text before it is replayed. */
static void check_replace(const char **lines, int nlines, const char *regex,
			  const char *repl, const char *want, int want_groups) {
	makeMinibuffer();
	struct buffer *buf = make_test_buffer_lines(lines, nlines);
	char *before = buffer_text(buf);

	int keys[64], n = 0;
	for (const char *c = regex; *c; c++)
		keys[n++] = *c;
	keys[n++] = '\r';
	for (const char *c = repl; *c; c++)
		keys[n++] = *c;
	keys[n++] = '\r';
	scriptKeys(keys, n);
	muteStdout();
	replaceRegex();
	unmuteStdout();
	clearKeys();

	char *after = buffer_text(buf);
	TEST_ASSERT_EQUAL_STRING(want, after);

	for (int g = 0; g < want_groups; g++) {
		TEST_ASSERT_TRUE(buf->undo != NULL);
		/* Only the head can be checked before replaying.  The
		 * group's other record is checked by doUndo itself. */
		assert_record_matches(buf, buf->undo, !buf->undo->delete);
		TEST_ASSERT_EQUAL_INT(UNDO_DONE, doUndo(buf, 1));
	}
	TEST_ASSERT_TRUE(buf->undo == NULL);
	char *undone = buffer_text(buf);
	TEST_ASSERT_EQUAL_STRING(before, undone);

	for (int g = 0; g < want_groups; g++) {
		TEST_ASSERT_TRUE(buf->redo != NULL);
		assert_record_matches(buf, buf->redo, buf->redo->delete);
		TEST_ASSERT_EQUAL_INT(UNDO_DONE, doRedo(buf, 1));
	}
	TEST_ASSERT_TRUE(buf->redo == NULL);
	char *redone = buffer_text(buf);
	TEST_ASSERT_EQUAL_STRING(want, redone);

	free(before);
	free(after);
	free(undone);
	free(redone);
	freeMinibuffer();
}

void test_replace_regex_one_row_history(void) {
	const char *lines[] = { "xaax yy", "" };
	check_replace(lines, 2, "a+", "Z", "xZx yy\n\n", 1);
}

/* The record spans from the first match to the last, across rows. */
void test_replace_regex_many_rows_history(void) {
	const char *lines[] = { "a1", "keep", "a2 tail", "" };
	check_replace(lines, 4, "a[0-9]", "Q", "Q\nkeep\nQ tail\n\n", 1);
}

/* An empty replacement is a pure delete, so a single record. */
void test_replace_regex_to_nothing_history(void) {
	const char *lines[] = { "xaay", "" };
	check_replace(lines, 2, "aa", "", "xy\n\n", 1);
}

/* Replacing the last row's text in a file with no final newline adds a
 * repair record, which undoes with the replacement. */
void test_replace_regex_final_row_history(void) {
	makeMinibuffer();
	struct buffer *buf = make_test_buffer("abc");
	bufferResetRows(buf);
	insertRow(buf, 0, (const uint8_t *)"abc", 3);
	int keys[] = { 'b', '\r', 'Z', '\r' };
	scriptKeys(keys, 4);
	muteStdout();
	replaceRegex();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("aZc", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(UNDO_DONE, doUndo(buf, 1));
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_TRUE(buf->undo == NULL);

	freeMinibuffer();
}

/* No match makes no record. */
void test_replace_regex_no_match_no_history(void) {
	makeMinibuffer();
	const char *lines[] = { "abc", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	int keys[] = { 'q', '\r', 'Z', '\r' };
	scriptKeys(keys, 4);
	muteStdout();
	replaceRegex();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_STRING("abc", row_str(buf, 0));
	TEST_ASSERT_TRUE(buf->undo == NULL);

	freeMinibuffer();
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_replace_regex_one_row_history);
	RUN_TEST(test_replace_regex_many_rows_history);
	RUN_TEST(test_replace_regex_to_nothing_history);
	RUN_TEST(test_replace_regex_final_row_history);
	RUN_TEST(test_replace_regex_no_match_no_history);

	RUN_TEST(test_replaces_every_match_on_a_line);
	RUN_TEST(test_replaces_across_multiple_lines);
	RUN_TEST(test_no_match_leaves_count_zero);

	RUN_TEST(test_span_excludes_untouched_head_and_tail);
	RUN_TEST(test_span_covers_gap_between_matches);

	RUN_TEST(test_whole_match_reference);
	RUN_TEST(test_numbered_groups);
	RUN_TEST(test_group_swap_repeats_per_match);
	RUN_TEST(test_nonparticipating_group_expands_empty);
	RUN_TEST(test_escaped_backslash_is_literal);

	RUN_TEST(test_valid_templates_accepted);
	RUN_TEST(test_unknown_escape_rejected);
	RUN_TEST(test_trailing_backslash_rejected);
	RUN_TEST(test_out_of_range_group_rejected);

	RUN_TEST(test_caret_anchors_at_each_line);
	RUN_TEST(test_dollar_anchors_at_each_line);
	RUN_TEST(test_notbol_suppresses_caret_at_subject_start);
	RUN_TEST(test_noteol_suppresses_dollar_at_subject_end);
	RUN_TEST(test_dot_does_not_cross_newline);
	RUN_TEST(test_literal_newline_in_pattern_matches);
	RUN_TEST(test_newline_in_bracket_expression);
	RUN_TEST(test_negated_class_does_not_match_newline);
	RUN_TEST(test_blank_line_pattern);
	RUN_TEST(test_newline_under_repetition);

	RUN_TEST(test_zero_width_match_terminates);
	RUN_TEST(test_zero_width_mixed_with_real_match);
	RUN_TEST(test_zero_width_step_preserves_utf8);
	RUN_TEST(test_end_anchor_zero_width_terminates);

	RUN_TEST(test_replacement_may_contain_utf8);
	RUN_TEST(test_match_may_contain_utf8);

	return TEST_END();
}
