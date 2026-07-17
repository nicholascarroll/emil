/* test_escape.c — Tests for escapePercent().
 *
 * escapePercent() doubles every '%' so user-controlled text can be
 * safely interpolated into a printf-style format string (e.g. an
 * editorPrompt() label).  Before this helper existed, the search /
 * replace and buffer-switch prompts built their format strings by
 * substituting raw user text, so a search term containing "%s" or
 * "%n" was interpreted as a conversion specifier when the prompt was
 * later passed to setStatusMessage() — a classic format-string bug
 * (crash via bogus %s dereference, memory write via %n).
 *
 * These tests lock in the escaping contract and, critically, verify
 * that a doubled-% string round-trips back to the literal original
 * when actually run through printf. */

#include "test.h"
#include "test_harness.h"
#include "util.h"
#include <string.h>
#include <stdio.h>

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- basic escaping ---- */

void test_escape_no_percent(void) {
	char out[32];
	size_t n = escapePercent(out, "hello world", sizeof(out));
	TEST_ASSERT_EQUAL_STRING("hello world", out);
	TEST_ASSERT_EQUAL_INT(11, (int)n);
}

void test_escape_empty(void) {
	char out[8];
	size_t n = escapePercent(out, "", sizeof(out));
	TEST_ASSERT_EQUAL_STRING("", out);
	TEST_ASSERT_EQUAL_INT(0, (int)n);
}

void test_escape_single_percent(void) {
	char out[8];
	escapePercent(out, "%", sizeof(out));
	TEST_ASSERT_EQUAL_STRING("%%", out);
}

void test_escape_percent_s(void) {
	char out[16];
	escapePercent(out, "%s", sizeof(out));
	TEST_ASSERT_EQUAL_STRING("%%s", out);
}

void test_escape_already_doubled(void) {
	char out[16];
	/* Each of the two '%' becomes '%%', so "%%" -> "%%%%". */
	escapePercent(out, "%%", sizeof(out));
	TEST_ASSERT_EQUAL_STRING("%%%%", out);
}

void test_escape_mixed(void) {
	char out[32];
	escapePercent(out, "a%b%%c", sizeof(out));
	TEST_ASSERT_EQUAL_STRING("a%%b%%%%c", out);
}

void test_escape_dangerous_specifiers(void) {
	char out[64];
	escapePercent(out, "%s%n%x%p", sizeof(out));
	TEST_ASSERT_EQUAL_STRING("%%s%%n%%x%%p", out);
}

/* ---- truncation must never split a "%%" pair ---- */

void test_escape_truncation_even(void) {
	/* Buffer holds "%%" + NUL exactly; the second '%' can't be
	 * doubled without room, so it's dropped whole (result stays a
	 * valid escaped string, never a dangling single '%'). */
	char out[4];
	size_t n = escapePercent(out, "%%%%", sizeof(out));
	TEST_ASSERT_EQUAL_STRING("%%", out);
	TEST_ASSERT_EQUAL_INT(2, (int)n);
	/* length is even -> no half-escaped '%' left behind */
	TEST_ASSERT((n % 2) == 0);
}

void test_escape_truncation_zero_size(void) {
	char out[1];
	out[0] = 'Z';
	size_t n = escapePercent(out, "%%%%", 0);
	/* dsize 0: nothing written, buffer untouched */
	TEST_ASSERT_EQUAL_INT(0, (int)n);
	TEST_ASSERT(out[0] == 'Z');
}

void test_escape_truncation_plain(void) {
	char out[4];
	size_t n = escapePercent(out, "abcdef", sizeof(out));
	TEST_ASSERT_EQUAL_STRING("abc", out);
	TEST_ASSERT_EQUAL_INT(3, (int)n);
}

/* ---- the whole point: printf round-trips to the literal ---- */

void test_escape_printf_roundtrip(void) {
	const char *inputs[] = {
		"%s%s%s%n",	 "100%",	  "%d apples",
		"a%%literal%%b", "plain text",	  "%",
		"%1$s%2$n",	 "tab\there",
	};
	for (size_t i = 0; i < sizeof(inputs) / sizeof(inputs[0]); i++) {
		char esc[128];
		escapePercent(esc, inputs[i], sizeof(esc));

		/* Feed the escaped string through printf as a *format*.
		 * If escaping is correct, the output equals the original
		 * literal.  The non-literal format is the whole point of
		 * the test, so silence the (correct) compiler warning. */
		char rendered[128];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
		int r = snprintf(rendered, sizeof(rendered), esc);
#pragma GCC diagnostic pop
		TEST_ASSERT(r >= 0);
		TEST_ASSERT_EQUAL_STRING(inputs[i], rendered);
	}
}

/* ---- emulate the real prompt-construction path ---- */

void test_escape_prompt_construction(void) {
	/* Mirrors find.c's queryReplace(): build a format from a
	 * user-controlled term, then render it with a single %s arg the
	 * way editorPrompt()->setStatusMessage() does. */
	const char *user_term = "%s%s%s%n";
	char esc[80];
	escapePercent(esc, user_term, sizeof(esc));

	char prompt[128];
	snprintf(prompt, sizeof(prompt), "Query replace %.78s with: %%s", esc);

	char status[128];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
	snprintf(status, sizeof(status), prompt, "TYPED");
#pragma GCC diagnostic pop

	TEST_ASSERT_EQUAL_STRING("Query replace %s%s%s%n with: TYPED", status);
}

void test_escape_capped_source_no_dangling(void) {
	/* The prompt sites cap the source length *before* escaping so a
	 * "%%" pair is never split.  Emulate that: a source that is all
	 * '%' capped to N chars then escaped must have an even count of
	 * consecutive '%' (i.e. every '%' is part of a complete pair). */
	char src[200];
	for (int i = 0; i < 199; i++)
		src[i] = '%';
	src[199] = '\0';

	char trunc[79]; /* cap to 78 chars */
	emil_strlcpy(trunc, src, sizeof(trunc));
	char esc[158];
	size_t n = escapePercent(esc, trunc, sizeof(esc));

	/* 78 source '%' -> 156 escaped '%' */
	TEST_ASSERT_EQUAL_INT(156, (int)n);
	int pct = 0;
	for (size_t i = 0; i < n; i++)
		if (esc[i] == '%')
			pct++;
	TEST_ASSERT((pct % 2) == 0);

	/* And it round-trips through printf without UB producing the
	 * literal 78 percent signs. */
	char rendered[128];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
#pragma GCC diagnostic ignored "-Wformat-security"
	int r = snprintf(rendered, sizeof(rendered), esc);
#pragma GCC diagnostic pop
	TEST_ASSERT(r == 78);
	for (int i = 0; i < 78; i++)
		TEST_ASSERT(rendered[i] == '%');
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_escape_no_percent);
	RUN_TEST(test_escape_empty);
	RUN_TEST(test_escape_single_percent);
	RUN_TEST(test_escape_percent_s);
	RUN_TEST(test_escape_already_doubled);
	RUN_TEST(test_escape_mixed);
	RUN_TEST(test_escape_dangerous_specifiers);
	RUN_TEST(test_escape_truncation_even);
	RUN_TEST(test_escape_truncation_zero_size);
	RUN_TEST(test_escape_truncation_plain);
	RUN_TEST(test_escape_printf_roundtrip);
	RUN_TEST(test_escape_prompt_construction);
	RUN_TEST(test_escape_capped_source_no_dangling);

	return TEST_END();
}
