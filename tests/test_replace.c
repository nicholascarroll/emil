/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_replace.c: replace-regexp.
 *
 * Every test runs the command the way a user does: M-x replace-regexp
 * RET, the pattern and the replacement typed at its two prompts, and
 * the buffer read afterwards.  Nothing here names the functions that
 * validate the replacement or do the substitution, so they can be
 * merged, renamed or made private without touching this file.
 *
 * Unless a test says otherwise the region is exactly the text under
 * test, so the pattern sees that text and nothing else: not the
 * buffer's final newline, and not the empty line after it.
 *
 * A newline in a pattern is typed with C-q C-j, so the cross-line
 * cases below are live behaviour, not hypotheticals. */

#include "test.h"
#include "test_harness.h"
#include "fileio.h"
#include "keymap.h"
#include "unicode.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void setUp(void) {
	initTestEditor();
	makeMinibuffer();
	setupCommands();
}
void tearDown(void) {
	freeMinibuffer();
	cleanupTestEditor();
}

/* ---- Keys ---- */

/* Append the keys that type `text` at a prompt.  A newline is typed
 * as C-q C-j.  A multibyte character arrives the way readKey()
 * delivers one: KEY_UNICODE, with its bytes waiting in E.unicode.
 * The stub cannot change those bytes between keys, so one script may
 * use only one multibyte character, however often. */
static int add_text(int *keys, int n, const char *text) {
	const unsigned char *p = (const unsigned char *)text;
	while (*p) {
		if (*p == '\n') {
			keys[n++] = KEY_CTRL('q');
			keys[n++] = KEY_CTRL('j');
			p++;
		} else if (*p < 0x80) {
			keys[n++] = *p++;
		} else {
			int len = (*p >= 0xF0) ? 4 : (*p >= 0xE0) ? 3 : 2;
			memcpy(E.unicode, p, len);
			E.nunicode = len;
			keys[n++] = KEY_UNICODE;
			p += len;
		}
	}
	return n;
}

/* M-x replace-regexp RET pattern RET replacement RET */
static void replace_regexp(const char *pattern, const char *replacement) {
	int keys[160];
	int n = 0;
	n = add_text(keys, n, "replace-regexp");
	keys[n++] = '\r';
	n = add_text(keys, n, pattern);
	keys[n++] = '\r';
	n = add_text(keys, n, replacement);
	keys[n++] = '\r';

	scriptKeys(keys, n);
	muteStdout();
	processKeypress(CMD_EXEC_CMD);
	unmuteStdout();
	/* Every key was wanted: the command asked for both answers. */
	TEST_ASSERT_EQUAL_INT(n, test_key_pos);
	clearKeys();
}

/* ---- Text ---- */

/* The whole buffer as the bytes a save would write. */
#define ASSERT_TEXT(expected, buf)                           \
	do {                                                 \
		size_t _len;                                 \
		char *_text = rowsToString((buf), &_len);    \
		TEST_ASSERT_EQUAL_STRING((expected), _text); \
		free(_text);                                 \
	} while (0)

/* A buffer holding `text` (lines separated by '\n', no final one),
 * with the region set from (sx, sy) to (ex, ey). */
static struct buffer *buffer_with_region(const char *text, int sx, int sy,
					 int ex, int ey) {
	char *copy = xstrdup(text);
	const char *lines[16];
	int n = 0;
	lines[n++] = copy;
	for (char *p = copy; *p; p++) {
		if (*p == '\n' && n < 16) {
			*p = 0;
			lines[n++] = p + 1;
		}
	}
	struct buffer *buf = make_test_buffer_lines(lines, n);
	free(copy);
	buf->markx = ex;
	buf->marky = ey;
	buf->mark_active = 1;
	buf->cx = sx;
	buf->cy = sy;
	return buf;
}

/* The same, with the region covering exactly the text. */
static struct buffer *buffer_all_selected(const char *text) {
	struct buffer *buf = buffer_with_region(text, 0, 0, 0, 0);
	int last = buf->numrows - 2; /* the row before the terminator */
	buf->markx = buf->row[last].size;
	buf->marky = last;
	return buf;
}

/* Replace in `text` and compare the buffer with `want`. */
static void check(const char *text, const char *pattern,
		  const char *replacement, const char *want) {
	struct buffer *buf = buffer_all_selected(text);
	replace_regexp(pattern, replacement);
	ASSERT_TEXT(want, buf);
}

/* The replacement is refused: nothing is replaced, though the pattern
 * matches, and the buffer is not marked modified. */
static void check_refused(const char *text, const char *pattern,
			  const char *replacement) {
	struct buffer *buf = buffer_all_selected(text);
	char *before;
	size_t len;
	before = rowsToString(buf, &len);
	replace_regexp(pattern, replacement);
	ASSERT_TEXT(before, buf);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
	TEST_ASSERT(strlen(E.statusmsg) > 0);
	free(before);
	/* Several of these run in one test. */
	destroyBuffer(buf);
	E.buf = NULL;
	E.headbuf = NULL;
	E.windows[0]->buf = NULL;
}

/* ---- The command end to end, with its history ---- */

/* Replace from the top of the buffer with no region, which means
 * "from point to the end"; then one undo must restore the text and
 * one redo reapply the replacement, however many matches there were. */
static void check_history(const char **lines, int nlines, const char *pattern,
			  const char *replacement, const char *want) {
	struct buffer *buf = make_test_buffer_lines(lines, nlines);
	size_t len;
	char *before = rowsToString(buf, &len);

	replace_regexp(pattern, replacement);
	ASSERT_TEXT(want, buf);
	TEST_ASSERT_TRUE(buf->dirty);

	processKeypress(CMD_UNDO);
	ASSERT_TEXT(before, buf);

	processKeypress(CMD_REDO);
	ASSERT_TEXT(want, buf);

	free(before);
}

void test_replace_regex_one_row_history(void) {
	const char *lines[] = { "xaax yy", "" };
	check_history(lines, 2, "a+", "Z", "xZx yy\n\n");
}

/* Matches on separate rows are still one step. */
void test_replace_regex_many_rows_history(void) {
	const char *lines[] = { "a1", "keep", "a2 tail", "" };
	check_history(lines, 4, "a[0-9]", "Q", "Q\nkeep\nQ tail\n\n");
}

/* An empty replacement is a pure delete, and one step too. */
void test_replace_regex_to_nothing_history(void) {
	const char *lines[] = { "xaay", "" };
	check_history(lines, 2, "aa", "", "xy\n\n");
}

/* Replacing the last row's text in a buffer with no final newline
 * repairs the newline, and the repair undoes with the replacement.
 *
 * The buffer is built by hand: neither loading a file nor typing
 * leaves a buffer without its final newline, so there are no keys
 * that make this one. */
void test_replace_regex_final_row_history(void) {
	struct buffer *buf = make_test_buffer("abc");
	bufferResetRows(buf);
	insertRow(buf, 0, (const uint8_t *)"abc", 3);
	buf->cx = 0;
	buf->cy = 0;
	ASSERT_TEXT("abc", buf);

	replace_regexp("b", "Z");
	ASSERT_TEXT("aZc\n", buf);

	processKeypress(CMD_UNDO);
	ASSERT_TEXT("abc", buf);
}

/* No match changes nothing, and leaves nothing to undo. */
void test_replace_regex_no_match_no_history(void) {
	const char *lines[] = { "abc", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);

	replace_regexp("q", "Z");
	ASSERT_TEXT("abc\n\n", buf);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	processKeypress(CMD_UNDO);
	ASSERT_TEXT("abc\n\n", buf);
}

/* ---- All matches, not just the first per line ---- */

void test_replaces_every_match_on_a_line(void) {
	check("banana", "a", "X", "bXnXnX\n");
}

/* ---- Capture groups and \& ---- */

void test_whole_match_reference(void) {
	check("a12b", "[0-9]+", "<\\&>", "a<12>b\n");
}

void test_numbered_groups(void) {
	check("key=42", "([a-z]+)=([0-9]+)", "\\2:\\1", "42:key\n");
}

void test_group_swap_repeats_per_match(void) {
	check("a1 b2", "([a-z])([0-9])", "\\2\\1", "1a 2b\n");
}

void test_nonparticipating_group_expands_empty(void) {
	/* The (x) alternative never participates, so \2 is empty. */
	check("a", "(a)|(x)", "[\\1\\2]", "[a]\n");
}

void test_escaped_backslash_is_literal(void) {
	check("a", "a", "\\\\", "\\\n");
}

/* ---- Replacements that are refused ---- */

void test_unknown_escape_rejected(void) {
	/* \n and \t are errors, not newline and tab. */
	check_refused("ab", "a", "a\\nb");
	check_refused("ab", "a", "a\\tb");
	check_refused("ab", "a", "\\q");
	/* \0 is not accepted as a synonym for \&. */
	check_refused("ab", "(a)", "\\0");
}

void test_trailing_backslash_rejected(void) {
	check_refused("ab", "a", "abc\\");
}

void test_out_of_range_group_rejected(void) {
	check_refused("ab", "(a)(b)", "\\3");
	check_refused("ab", "a", "\\1");
	/* The highest group the pattern has is accepted. */
	check("ab", "(a)(b)", "\\2", "b\n");
}

/* ---- Anchoring ---- */

void test_caret_anchors_at_each_line(void) {
	check("abc\nabc", "^a", "X", "Xbc\nXbc\n");
}

void test_dollar_anchors_at_each_line(void) {
	check("abc\nabc", "c$", "X", "abX\nabX\n");
}

void test_notbol_suppresses_caret_at_subject_start(void) {
	/* The region starts after the "x": its first "a" is not at the
	 * start of a line, so ^ must not match it.  The "a" that opens
	 * the second line still matches. */
	struct buffer *buf = buffer_with_region("xabc\nabc", 1, 0, 3, 1);
	replace_regexp("^a", "X");
	ASSERT_TEXT("xabc\nXbc\n", buf);
}

void test_noteol_suppresses_dollar_at_subject_end(void) {
	/* The region ends before the "x": its last "c" is not at the
	 * end of a line, so $ must not match it. */
	struct buffer *buf = buffer_with_region("abc\nabcx", 0, 0, 3, 1);
	replace_regexp("c$", "X");
	ASSERT_TEXT("abX\nabcx\n", buf);
}

void test_dot_does_not_cross_newline(void) {
	struct buffer *buf = buffer_all_selected("a\nc");
	replace_regexp("a.c", "X");
	ASSERT_TEXT("a\nc\n", buf);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}

/* Patterns containing a literal newline.  These are the portability
 * surface, because REG_NEWLINE is where libc implementations could
 * plausibly differ.  Verified identical on glibc and musl; run the
 * suite on macOS, MSYS2 and bionic to confirm the POSIX guarantee
 * holds there too. */
void test_literal_newline_in_pattern_matches(void) {
	check("ab\ncd", "b\nc", "X", "aXd\n");
}

void test_newline_in_bracket_expression(void) {
	check("ab\ncd", "b[\n]c", "X", "aXd\n");
}

void test_blank_line_pattern(void) {
	check("a\n\nb", "\n\n", "X", "aXb\n");
}

void test_newline_under_repetition(void) {
	check("a\n\n\nb", "a\n+b", "X", "X\n");
}

/* ---- Zero-width matches ---- */

void test_zero_width_match_terminates(void) {
	/* x* matches empty at every position; without an explicit
	 * step this would spin forever. */
	check("ab", "x*", "-", "-a-b-\n");
}

void test_zero_width_mixed_with_real_match(void) {
	/* The empty match sitting immediately after the "a" must not
	 * count as its own occurrence, or this yields -b--b-. */
	check("bab", "a*", "-", "-b-b-\n");
}

void test_zero_width_step_preserves_utf8(void) {
	/* Stepping one byte instead of one character would put a "-"
	 * inside the two-byte é and corrupt the buffer. */
	struct buffer *buf = buffer_all_selected("caf\xc3\xa9");
	replace_regexp("x*", "-");
	ASSERT_TEXT("-c-a-f-\xc3\xa9-\n", buf);
	TEST_ASSERT(utf8_validate(buf->row[0].chars, buf->row[0].size));
}

void test_end_anchor_zero_width_terminates(void) {
	check("ab", "$", "!", "ab!\n");
}

/* ---- Multibyte payloads ---- */

void test_replacement_may_contain_utf8(void) {
	check("cafe", "cafe", "caf\xc3\xa9", "caf\xc3\xa9\n");
}

void test_match_may_contain_utf8(void) {
	check("a caf\xc3\xa9 b", "caf\xc3\xa9", "<\\&>",
	      "a <caf\xc3\xa9> b\n");
}

/* ---- Runner ---- */

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_replace_regex_one_row_history);
	RUN_TEST(test_replace_regex_many_rows_history);
	RUN_TEST(test_replace_regex_to_nothing_history);
	RUN_TEST(test_replace_regex_final_row_history);
	RUN_TEST(test_replace_regex_no_match_no_history);

	RUN_TEST(test_replaces_every_match_on_a_line);

	RUN_TEST(test_whole_match_reference);
	RUN_TEST(test_numbered_groups);
	RUN_TEST(test_group_swap_repeats_per_match);
	RUN_TEST(test_nonparticipating_group_expands_empty);
	RUN_TEST(test_escaped_backslash_is_literal);

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
