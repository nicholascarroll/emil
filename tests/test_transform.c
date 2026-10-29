/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_transform.c: Latin Extended case mapping in transform functions. */

#include "test.h"
#include "test_harness.h"
#include "transform.h"
#include "region.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ---- Upcase ---- */

void test_upcase_ascii(void) {
	uint8_t *r = transformerUpcase((uint8_t *)"hello");
	TEST_ASSERT_EQUAL_STRING("HELLO", (char *)r);
	free(r);
}

void test_upcase_cafe(void) {
	uint8_t *r = transformerUpcase((uint8_t *)"caf\xc3\xa9");
	TEST_ASSERT_EQUAL_STRING("CAF\xc3\x89", (char *)r);
	free(r);
}

void test_upcase_eszett_passthrough(void) {
	/* ß (C3 9F) should pass through unchanged */
	uint8_t *r = transformerUpcase((uint8_t *)"\xc3\x9f");
	TEST_ASSERT_EQUAL_STRING("\xc3\x9f", (char *)r);
	free(r);
}

/* ---- Downcase ---- */

void test_downcase_ascii(void) {
	uint8_t *r = transformerDowncase((uint8_t *)"HELLO");
	TEST_ASSERT_EQUAL_STRING("hello", (char *)r);
	free(r);
}

void test_downcase_cafe(void) {
	uint8_t *r = transformerDowncase((uint8_t *)"CAF\xc3\x89");
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", (char *)r);
	free(r);
}

/* ---- Capital case ---- */

void test_capital_ascii(void) {
	uint8_t *r = transformerCapitalCase((uint8_t *)"hello world");
	TEST_ASSERT_EQUAL_STRING("Hello World", (char *)r);
	free(r);
}

void test_capital_cafe(void) {
	uint8_t *r = transformerCapitalCase((uint8_t *)"caf\xc3\xa9");
	TEST_ASSERT_EQUAL_STRING("Caf\xc3\xa9", (char *)r);
	free(r);
}

void test_capital_mixed_words(void) {
	/* "hello café world" → "Hello Café World" */
	uint8_t *r =
		transformerCapitalCase((uint8_t *)"hello caf\xc3\xa9 world");
	TEST_ASSERT_EQUAL_STRING("Hello Caf\xc3\xa9 World", (char *)r);
	free(r);
}

void test_capital_leading_accent(void) {
	/* élan → Élan */
	uint8_t *r = transformerCapitalCase((uint8_t *)"\xc3\xa9lan");
	TEST_ASSERT_EQUAL_STRING("\xc3\x89lan", (char *)r);
	free(r);
}

/* ---- Latin Extended-A ---- */

void test_upcase_amacron(void) {
	/* ā (C4 81) → Ā (C4 80) */
	uint8_t *r = transformerUpcase((uint8_t *)"\xc4\x81");
	TEST_ASSERT_EQUAL_STRING("\xc4\x80", (char *)r);
	free(r);
}

void test_downcase_amacron(void) {
	/* Ā (C4 80) → ā (C4 81) */
	uint8_t *r = transformerDowncase((uint8_t *)"\xc4\x80");
	TEST_ASSERT_EQUAL_STRING("\xc4\x81", (char *)r);
	free(r);
}

/* ---- Passthrough of non-letter symbols ---- */

void test_upcase_multiply_sign(void) {
	/* × (C3 97 = U+00D7) should pass through unchanged */
	uint8_t *r = transformerUpcase((uint8_t *)"a\xc3\x97"
						  "b");
	TEST_ASSERT_EQUAL_STRING("A\xc3\x97"
				 "B",
				 (char *)r);
	free(r);
}

void test_downcase_divide_sign(void) {
	/* ÷ (C3 B7 = U+00F7) should pass through unchanged */
	uint8_t *r = transformerDowncase((uint8_t *)"A\xc3\xb7"
						    "B");
	TEST_ASSERT_EQUAL_STRING("a\xc3\xb7"
				 "b",
				 (char *)r);
	free(r);
}

/* ---- Latin Extended-A, the whole block ----
 *
 * Each capital sits just before its small letter, but the capital is
 * on the even codepoint in U+0100–U+0137 and U+014A–U+0177 and on the
 * odd one in U+0139–U+0148 and U+0179–U+017E.  Treating the block as
 * one parity turned ł into Ń and Ł into ŀ, and left ź ż ž alone.
 *
 * The pairs below come from the Unicode case mappings, not from the
 * code under test. */

static const struct {
	uint16_t upper, lower;
} latinExtAPairs[] = {
	{ 0x0100, 0x0101 }, { 0x0102, 0x0103 }, { 0x0104, 0x0105 },
	{ 0x0106, 0x0107 }, { 0x0108, 0x0109 }, { 0x010A, 0x010B },
	{ 0x010C, 0x010D }, { 0x010E, 0x010F }, { 0x0110, 0x0111 },
	{ 0x0112, 0x0113 }, { 0x0114, 0x0115 }, { 0x0116, 0x0117 },
	{ 0x0118, 0x0119 }, { 0x011A, 0x011B }, { 0x011C, 0x011D },
	{ 0x011E, 0x011F }, { 0x0120, 0x0121 }, { 0x0122, 0x0123 },
	{ 0x0124, 0x0125 }, { 0x0126, 0x0127 }, { 0x0128, 0x0129 },
	{ 0x012A, 0x012B }, { 0x012C, 0x012D }, { 0x012E, 0x012F },
	{ 0x0132, 0x0133 }, { 0x0134, 0x0135 }, { 0x0136, 0x0137 },
	{ 0x0139, 0x013A }, { 0x013B, 0x013C }, { 0x013D, 0x013E },
	{ 0x013F, 0x0140 }, { 0x0141, 0x0142 }, { 0x0143, 0x0144 },
	{ 0x0145, 0x0146 }, { 0x0147, 0x0148 }, { 0x014A, 0x014B },
	{ 0x014C, 0x014D }, { 0x014E, 0x014F }, { 0x0150, 0x0151 },
	{ 0x0152, 0x0153 }, { 0x0154, 0x0155 }, { 0x0156, 0x0157 },
	{ 0x0158, 0x0159 }, { 0x015A, 0x015B }, { 0x015C, 0x015D },
	{ 0x015E, 0x015F }, { 0x0160, 0x0161 }, { 0x0162, 0x0163 },
	{ 0x0164, 0x0165 }, { 0x0166, 0x0167 }, { 0x0168, 0x0169 },
	{ 0x016A, 0x016B }, { 0x016C, 0x016D }, { 0x016E, 0x016F },
	{ 0x0170, 0x0171 }, { 0x0172, 0x0173 }, { 0x0174, 0x0175 },
	{ 0x0176, 0x0177 }, { 0x0178, 0x00FF }, { 0x0179, 0x017A },
	{ 0x017B, 0x017C }, { 0x017D, 0x017E },
};

/* Two-byte UTF-8 for a codepoint below U+0800, NUL-terminated. */
static void enc2(uint16_t cp, char out[3]) {
	out[0] = (char)(0xC0 | (cp >> 6));
	out[1] = (char)(0x80 | (cp & 0x3F));
	out[2] = '\0';
}

static int caseMapIs(uint8_t *(*fn)(uint8_t *), uint16_t from, uint16_t to) {
	char in[3], want[3];
	enc2(from, in);
	enc2(to, want);
	uint8_t *r = fn((uint8_t *)in);
	int ok = strcmp((char *)r, want) == 0;
	if (!ok)
		printf("    U+%04X -> %02X %02X, want U+%04X\n", from, r[0],
		       r[1], to);
	free(r);
	return ok;
}

void test_latin_extended_a_case_pairs(void) {
	size_t n = sizeof(latinExtAPairs) / sizeof(latinExtAPairs[0]);
	for (size_t i = 0; i < n; i++) {
		uint16_t up = latinExtAPairs[i].upper;
		uint16_t lo = latinExtAPairs[i].lower;
		TEST_ASSERT_TRUE(caseMapIs(transformerUpcase, lo, up));
		TEST_ASSERT_TRUE(caseMapIs(transformerUpcase, up, up));
		TEST_ASSERT_TRUE(caseMapIs(transformerDowncase, up, lo));
		TEST_ASSERT_TRUE(caseMapIs(transformerDowncase, lo, lo));
	}
}

/* No partner of the same encoded length, so no change either way:
 * İ ı (Turkish, whose partners are ASCII), ĸ, ŉ and ſ. */
void test_latin_extended_a_unpaired_unchanged(void) {
	static const uint16_t lone[] = { 0x0130, 0x0131, 0x0138, 0x0149,
					 0x017F };
	for (size_t i = 0; i < sizeof(lone) / sizeof(lone[0]); i++) {
		TEST_ASSERT_TRUE(
			caseMapIs(transformerUpcase, lone[i], lone[i]));
		TEST_ASSERT_TRUE(
			caseMapIs(transformerDowncase, lone[i], lone[i]));
	}
}

/* The Polish pangram, both ways, and M-c on a word that starts with
 * one of the letters that used to change identity. */
void test_polish_text_round_trips(void) {
	const char *lower = "za\xc5\xbc\xc3\xb3\xc5\x82\xc4\x87 "
			    "g\xc4\x99\xc5\x9bl\xc4\x85 "
			    "ja\xc5\xba\xc5\x84";
	const char *upper = "ZA\xc5\xbb\xc3\x93\xc5\x81\xc4\x86 "
			    "G\xc4\x98\xc5\x9aL\xc4\x84 "
			    "JA\xc5\xb9\xc5\x83";

	uint8_t *r = transformerUpcase((uint8_t *)lower);
	TEST_ASSERT_EQUAL_STRING(upper, (char *)r);
	free(r);
	r = transformerDowncase((uint8_t *)upper);
	TEST_ASSERT_EQUAL_STRING(lower, (char *)r);
	free(r);

	/* łódź -> Łódź */
	r = transformerCapitalCase((uint8_t *)"\xc5\x82\xc3\xb3"
					      "d\xc5\xba");
	TEST_ASSERT_EQUAL_STRING("\xc5\x81\xc3\xb3"
				 "d\xc5\xba",
				 (char *)r);
	free(r);
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}


/* ---- Transpose regressions ---- */

/* Regression: a region containing a single word (no boundary before
 * a second word) made startSecond - endFirst negative, passing a
 * huge size_t to memcpy (M-t crash on one-word buffers). */
void test_transpose_words_single_word(void) {
	uint8_t *r = transformerTransposeWords((uint8_t *)"hello");
	TEST_ASSERT_EQUAL_STRING("hello", (char *)r);
	free(r);
	r = transformerTransposeWords((uint8_t *)"hello ");
	TEST_ASSERT_EQUAL_STRING("hello ", (char *)r);
	free(r);
}

void test_transpose_words_two_words(void) {
	uint8_t *r = transformerTransposeWords((uint8_t *)"foo bar");
	TEST_ASSERT_EQUAL_STRING("bar foo", (char *)r);
	free(r);
}

void test_transpose_chars_empty(void) {
	uint8_t *r = transformerTransposeChars((uint8_t *)"");
	TEST_ASSERT_EQUAL_STRING("", (char *)r);
	free(r);
}


/* Regression: a transformer returning NULL (transformerPipeCmd does,
 * when the subprocess can't be spawned or joined) crashed
 * transformRange with strlen(NULL).  The buffer must stay untouched. */
static uint8_t *nullTransformer(uint8_t *in) {
	(void)in;
	return NULL;
}

void test_transform_region_null_transformer(void) {
	struct buffer *buf = make_test_buffer("hello world");
	buf->cx = 0;
	buf->cy = 0;
	buf->markx = 5;
	buf->marky = 0;
	buf->mark_active = 1;
	transformRegion(nullTransformer);
	TEST_ASSERT_EQUAL_STRING("hello world", row_str(buf, 0));
}

int main(void) {
	TEST_BEGIN();

	/* Upcase */
	RUN_TEST(test_upcase_ascii);
	RUN_TEST(test_upcase_cafe);
	RUN_TEST(test_upcase_eszett_passthrough);

	/* Downcase */
	RUN_TEST(test_downcase_ascii);
	RUN_TEST(test_downcase_cafe);

	/* Capital case */
	RUN_TEST(test_capital_ascii);
	RUN_TEST(test_capital_cafe);
	RUN_TEST(test_capital_mixed_words);
	RUN_TEST(test_capital_leading_accent);

	/* Round-trip */

	/* Latin Extended-A */
	RUN_TEST(test_upcase_amacron);
	RUN_TEST(test_downcase_amacron);

	/* Symbol passthrough */
	RUN_TEST(test_upcase_multiply_sign);
	RUN_TEST(test_downcase_divide_sign);

	/* Latin Extended-A, the whole block */
	RUN_TEST(test_latin_extended_a_case_pairs);
	RUN_TEST(test_latin_extended_a_unpaired_unchanged);
	RUN_TEST(test_polish_text_round_trips);

	RUN_TEST(test_transpose_words_single_word);
	RUN_TEST(test_transpose_words_two_words);
	RUN_TEST(test_transpose_chars_empty);
	RUN_TEST(test_transform_region_null_transformer);

	return TEST_END();
}
