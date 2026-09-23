/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_utf8_validate.c: Tests for the utf8_validate() utility. */

#include "test.h"
#include "test_harness.h"
#include "unicode.h"
#include <stdint.h>
#include <string.h>

/* The zero-length string.  Every non-empty case is covered by the
 * exhaustive sweep below, which enumerates lengths 1 to 4. */
void test_valid_empty(void) {
	uint8_t s[] = { 0 };
	TEST_ASSERT_TRUE(utf8_validate(s, 0));
}

void setUp(void) {
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ----------------------------------------------------------------
 * Exhaustive differential verification
 *
 * These compare utf8_validate() against an independent oracle over the
 * ENTIRE sequence space, so its correctness rests on enumeration
 * rather than on having thought of every corner.  Hand-picked cases
 * for overlongs, surrogates, truncations and bad continuations used to
 * sit above; the sweep subsumes all of them.
 *
 * Oracle: the Unicode Standard's Table 3-7, "Well-Formed UTF-8 Byte
 * Sequences", transliterated verbatim as byte-range checks -- no
 * decoding, no arithmetic, nothing shared with the implementation
 * under test.  emil's one deliberate deviation from the standard
 * (NUL bytes are forbidden in buffers) is applied in the oracle's
 * outer loop, not in the table.
 *
 * Coverage argument: utf8_validate consumes one sequence per loop
 * iteration with a strictly increasing index, so whole-string
 * correctness follows from per-sequence correctness by induction.
 * The per-sequence space is enumerated completely: all 1-, 2- and
 * 3-byte strings, and all 4-byte strings whose lead byte is
 * 0xF0..0xFF (a 4-byte string with any other lead begins with a
 * sequence of length <= 3, which the shorter enumerations already
 * cover, truncations included).  The randomized differential test
 * then exercises the multi-sequence loop itself.
 * ---------------------------------------------------------------- */

static int in_range(uint8_t b, uint8_t lo, uint8_t hi) {
	return lo <= b && b <= hi;
}

/* Length of the well-formed UTF-8 sequence beginning at p (1-4), or
 * 0 if p does not begin with one.  Verbatim Table 3-7. */
static int oracle_seq(const uint8_t *p, int n) {
	uint8_t b0 = p[0];
	if (b0 <= 0x7F)
		return 1;
	if (in_range(b0, 0xC2, 0xDF))
		return (n >= 2 && in_range(p[1], 0x80, 0xBF)) ? 2 : 0;
	if (b0 == 0xE0)
		return (n >= 3 && in_range(p[1], 0xA0, 0xBF) &&
			in_range(p[2], 0x80, 0xBF)) ?
			       3 :
			       0;
	if (in_range(b0, 0xE1, 0xEC))
		return (n >= 3 && in_range(p[1], 0x80, 0xBF) &&
			in_range(p[2], 0x80, 0xBF)) ?
			       3 :
			       0;
	if (b0 == 0xED)
		return (n >= 3 && in_range(p[1], 0x80, 0x9F) &&
			in_range(p[2], 0x80, 0xBF)) ?
			       3 :
			       0;
	if (in_range(b0, 0xEE, 0xEF))
		return (n >= 3 && in_range(p[1], 0x80, 0xBF) &&
			in_range(p[2], 0x80, 0xBF)) ?
			       3 :
			       0;
	if (b0 == 0xF0)
		return (n >= 4 && in_range(p[1], 0x90, 0xBF) &&
			in_range(p[2], 0x80, 0xBF) &&
			in_range(p[3], 0x80, 0xBF)) ?
			       4 :
			       0;
	if (in_range(b0, 0xF1, 0xF3))
		return (n >= 4 && in_range(p[1], 0x80, 0xBF) &&
			in_range(p[2], 0x80, 0xBF) &&
			in_range(p[3], 0x80, 0xBF)) ?
			       4 :
			       0;
	if (b0 == 0xF4)
		return (n >= 4 && in_range(p[1], 0x80, 0x8F) &&
			in_range(p[2], 0x80, 0xBF) &&
			in_range(p[3], 0x80, 0xBF)) ?
			       4 :
			       0;
	return 0; /* C0, C1, F5..FF: no Table 3-7 row */
}

static int oracle_validate(const uint8_t *buf, int len) {
	int i = 0;
	while (i < len) {
		if (buf[i] == 0x00)
			return 0; /* emil policy: no NUL in buffers */
		int k = oracle_seq(&buf[i], len - i);
		if (k == 0)
			return 0;
		i += k;
	}
	return 1;
}

/* Report at most a few mismatches, not millions of lines. */
static long _mismatches;

static void report_mismatch(const uint8_t *buf, int len, int impl,
			    int oracle) {
	_mismatches++;
	if (_mismatches <= 5) {
		printf("  FAIL: impl=%d oracle=%d for bytes:", impl, oracle);
		for (int i = 0; i < len; i++)
			printf(" %02X", buf[i]);
		printf("\n");
	}
	_current_test_failed = 1;
}

static void diff_check(const uint8_t *buf, int len) {
	int impl = utf8_validate(buf, len);
	int oracle = oracle_validate(buf, len);
	if (impl != oracle)
		report_mismatch(buf, len, impl, oracle);
}

/* Pin the oracle itself against hand-computed vectors from the
 * standard, so a transliteration typo cannot silently agree with a
 * matching implementation bug. */
void test_oracle_self_check(void) {
	uint8_t ok2[] = { 0xC2, 0x80 };		 /* U+0080 minimum */
	uint8_t ok3[] = { 0xE0, 0xA0, 0x80 };	 /* U+0800 minimum */
	uint8_t sur[] = { 0xED, 0xA0, 0x80 };	 /* U+D800 surrogate */
	uint8_t max[] = { 0xF4, 0x8F, 0xBF, 0xBF }; /* U+10FFFF */
	uint8_t ovr[] = { 0xF4, 0x90, 0x80, 0x80 }; /* U+110000 */
	uint8_t o2[] = { 0xC1, 0xBF };		 /* overlong U+007F */
	uint8_t o4[] = { 0xF0, 0x8F, 0xBF, 0xBF }; /* overlong U+FFFF */
	uint8_t nul[] = { 'A', 0x00, 'B' };
	TEST_ASSERT_EQUAL_INT(1, oracle_validate(ok2, 2));
	TEST_ASSERT_EQUAL_INT(1, oracle_validate(ok3, 3));
	TEST_ASSERT_EQUAL_INT(0, oracle_validate(sur, 3));
	TEST_ASSERT_EQUAL_INT(1, oracle_validate(max, 4));
	TEST_ASSERT_EQUAL_INT(0, oracle_validate(ovr, 4));
	TEST_ASSERT_EQUAL_INT(0, oracle_validate(o2, 2));
	TEST_ASSERT_EQUAL_INT(0, oracle_validate(o4, 4));
	TEST_ASSERT_EQUAL_INT(0, oracle_validate(nul, 3));
}

void test_exhaustive_1byte(void) {
	_mismatches = 0;
	uint8_t buf[1];
	for (int b0 = 0; b0 < 256; b0++) {
		buf[0] = (uint8_t)b0;
		diff_check(buf, 1);
	}
}

void test_exhaustive_2byte(void) {
	_mismatches = 0;
	uint8_t buf[2];
	for (int b0 = 0; b0 < 256; b0++) {
		buf[0] = (uint8_t)b0;
		for (int b1 = 0; b1 < 256; b1++) {
			buf[1] = (uint8_t)b1;
			diff_check(buf, 2);
		}
	}
}

void test_exhaustive_3byte(void) {
	_mismatches = 0;
	uint8_t buf[3];
	for (int b0 = 0; b0 < 256; b0++) {
		buf[0] = (uint8_t)b0;
		for (int b1 = 0; b1 < 256; b1++) {
			buf[1] = (uint8_t)b1;
			for (int b2 = 0; b2 < 256; b2++) {
				buf[2] = (uint8_t)b2;
				diff_check(buf, 3);
			}
		}
	}
}

void test_exhaustive_4byte_leads(void) {
	_mismatches = 0;
	uint8_t buf[4];
	for (int b0 = 0xF0; b0 < 0x100; b0++) {
		buf[0] = (uint8_t)b0;
		for (int b1 = 0; b1 < 256; b1++) {
			buf[1] = (uint8_t)b1;
			for (int b2 = 0; b2 < 256; b2++) {
				buf[2] = (uint8_t)b2;
				for (int b3 = 0; b3 < 256; b3++) {
					buf[3] = (uint8_t)b3;
					diff_check(buf, 4);
				}
			}
		}
	}
}

/* ---- Multi-sequence composition ----
 *
 * The exhaustive tests above establish per-sequence correctness;
 * these exercise the multi-sequence loop.  Deterministic xorshift
 * PRNG with a fixed seed: same inputs every run, no flakiness. */

static uint32_t _rng;

static uint32_t rnd(void) {
	_rng ^= _rng << 13;
	_rng ^= _rng >> 17;
	_rng ^= _rng << 5;
	return _rng;
}

/* Append one random well-formed sequence to buf at position n.
 * Returns the new length.  Sampled per Table 3-7 rows, biased
 * toward row boundaries where mistakes live. */
static int append_valid_seq(uint8_t *buf, int n) {
	switch (rnd() % 4) {
	case 0:
		buf[n++] = 0x01 + (uint8_t)(rnd() % 0x7F); /* ASCII, no NUL */
		break;
	case 1:
		buf[n++] = 0xC2 + (uint8_t)(rnd() % (0xDF - 0xC2 + 1));
		buf[n++] = 0x80 + (uint8_t)(rnd() % 0x40);
		break;
	case 2: {
		uint8_t b0 = 0xE0 + (uint8_t)(rnd() % 0x10);
		uint8_t lo1 = (b0 == 0xE0) ? 0xA0 : 0x80;
		uint8_t hi1 = (b0 == 0xED) ? 0x9F : 0xBF;
		buf[n++] = b0;
		buf[n++] = lo1 + (uint8_t)(rnd() % (hi1 - lo1 + 1));
		buf[n++] = 0x80 + (uint8_t)(rnd() % 0x40);
		break;
	}
	default: {
		uint8_t b0 = 0xF0 + (uint8_t)(rnd() % 5);
		uint8_t lo1 = (b0 == 0xF0) ? 0x90 : 0x80;
		uint8_t hi1 = (b0 == 0xF4) ? 0x8F : 0xBF;
		buf[n++] = b0;
		buf[n++] = lo1 + (uint8_t)(rnd() % (hi1 - lo1 + 1));
		buf[n++] = 0x80 + (uint8_t)(rnd() % 0x40);
		buf[n++] = 0x80 + (uint8_t)(rnd() % 0x40);
		break;
	}
	}
	return n;
}

void test_composition_valid_concatenations(void) {
	_mismatches = 0;
	_rng = 0x9E3779B9u;
	uint8_t buf[64 + 4];
	for (int iter = 0; iter < 20000; iter++) {
		int n = 0;
		int nseq = 1 + (int)(rnd() % 12);
		for (int s = 0; s < nseq && n < 60; s++)
			n = append_valid_seq(buf, n);
		/* A concatenation of well-formed sequences is valid:
		 * both sides must agree AND the verdict must be 1. */
		if (!utf8_validate(buf, n) || !oracle_validate(buf, n))
			report_mismatch(buf, n, utf8_validate(buf, n),
					oracle_validate(buf, n));
	}
}

void test_composition_differential_mutated(void) {
	_mismatches = 0;
	_rng = 0xB5297A4Du;
	uint8_t buf[64 + 4];
	for (int iter = 0; iter < 40000; iter++) {
		int n = 0;
		int nseq = 1 + (int)(rnd() % 12);
		for (int s = 0; s < nseq && n < 60; s++)
			n = append_valid_seq(buf, n);
		/* Corrupt 1-2 positions with arbitrary bytes (NUL
		 * included), then require impl == oracle whatever
		 * the verdict. */
		int hits = 1 + (int)(rnd() % 2);
		for (int h = 0; h < hits; h++)
			buf[rnd() % (uint32_t)n] = (uint8_t)(rnd() % 256);
		diff_check(buf, n);
	}
}

void test_composition_differential_random(void) {
	_mismatches = 0;
	_rng = 0x1B873593u;
	uint8_t buf[64];
	for (int iter = 0; iter < 40000; iter++) {
		int n = 1 + (int)(rnd() % 64);
		for (int i = 0; i < n; i++)
			buf[i] = (uint8_t)(rnd() % 256);
		diff_check(buf, n);
	}
}

/* ----------------------------------------------------------------
 * utf8SanitizeLine: untrusted bytes to one safe line of status text
 * ---------------------------------------------------------------- */

static const char *sanitized(const char *in, size_t len) {
	static char out[256];
	utf8SanitizeLine((const uint8_t *)in, len, out, sizeof(out));
	return out;
}

/* A string literal's bytes, less the terminating NUL. */
#define SANITIZED(lit) sanitized((lit), sizeof(lit) - 1)

void test_sanitize_examples(void) {
	TEST_ASSERT_EQUAL_STRING("plain text", SANITIZED("plain text"));
	/* C0 controls and DEL in caret notation: nothing reaches the
	 * terminal that it would act on. */
	TEST_ASSERT_EQUAL_STRING("^[[31mred^[[0m",
				 SANITIZED("\033[31mred\033[0m"));
	TEST_ASSERT_EQUAL_STRING("a^Jb^Ic^M^?", SANITIZED("a\nb\tc\r\177"));
	TEST_ASSERT_EQUAL_STRING("a^@b", SANITIZED("a\0b"));
	/* Valid UTF-8 passes through untouched. */
	TEST_ASSERT_EQUAL_STRING("caf\xC3\xA9 \xE4\xB8\xAD \xF0\x9F\x98\x80",
				 SANITIZED("caf\xC3\xA9 \xE4\xB8\xAD "
					   "\xF0\x9F\x98\x80"));
	/* C1 controls -- U+009B is CSI to an 8-bit terminal. */
	TEST_ASSERT_EQUAL_STRING("x?y", SANITIZED("x\xC2\x9By"));
	/* Each byte that does not start a valid sequence: U+FFFD. */
	TEST_ASSERT_EQUAL_STRING("\xEF\xBF\xBDx", SANITIZED("\xFFx"));
	TEST_ASSERT_EQUAL_STRING("\xEF\xBF\xBD\xEF\xBF\xBD",
				 SANITIZED("\xE4\xB8")); /* truncated */
	TEST_ASSERT_EQUAL_STRING("\xEF\xBF\xBD\xEF\xBF\xBD",
				 SANITIZED("\xC0\xAF")); /* overlong */
}

/* A full output buffer stops before a character, never inside one, and
 * reports how much of the input it used. */
void test_sanitize_stops_on_a_character_boundary(void) {
	char out[5];
	size_t used = utf8SanitizeLine((const uint8_t *)"ab\xE4\xB8\xAD", 5,
				       out, sizeof(out));
	TEST_ASSERT_EQUAL_STRING("ab", out);
	TEST_ASSERT_EQUAL_INT(2, (int)used);

	used = utf8SanitizeLine((const uint8_t *)"abc\033", 4, out,
				sizeof(out));
	TEST_ASSERT_EQUAL_STRING("abc", out); /* no room for "^[" */
	TEST_ASSERT_EQUAL_INT(3, (int)used);

	TEST_ASSERT_EQUAL_INT(0, (int)utf8SanitizeLine((const uint8_t *)"a", 1,
						       out, 0));
}

/* What makes the output safe, checked over every one- and two-byte
 * input and a spread of random longer ones: it is valid UTF-8, holds
 * no C0 or C1 control and no DEL, and consumes all of its input when
 * there is room. */
static int sanitizedIsSafe(const uint8_t *in, size_t len) {
	char out[16 * 3 + 1];
	size_t used = utf8SanitizeLine(in, len, out, sizeof(out));
	size_t olen = strlen(out);
	if (used != len)
		return 0;
	if (olen > 0 && !utf8_validate((const uint8_t *)out, (int)olen))
		return 0;
	for (size_t i = 0; i < olen;) {
		uint8_t c = (uint8_t)out[i];
		if (c < 0x20 || c == 0x7F)
			return 0;
		int n = utf8_nBytes(c);
		if (n > 1 && utf8Decode((const uint8_t *)out, (int)i) <= 0x9F)
			return 0;
		i += (size_t)n;
	}
	return 1;
}

void test_sanitize_output_is_always_safe(void) {
	uint8_t in[16];
	int bad = 0;
	for (int a = 0; a < 256; a++) {
		in[0] = (uint8_t)a;
		bad += !sanitizedIsSafe(in, 1);
		for (int b = 0; b < 256; b++) {
			in[1] = (uint8_t)b;
			bad += !sanitizedIsSafe(in, 2);
		}
	}
	uint32_t x = 2463534242u; /* xorshift32 */
	for (int t = 0; t < 200000; t++) {
		size_t len = 1 + t % 16;
		for (size_t i = 0; i < len; i++) {
			x ^= x << 13;
			x ^= x >> 17;
			x ^= x << 5;
			/* Bias toward UTF-8's interesting bytes. */
			in[i] = (x & 1) ? (uint8_t)(0x80 | (x >> 8 & 0x7F)) :
					  (uint8_t)(x >> 8);
		}
		bad += !sanitizedIsSafe(in, len);
	}
	TEST_ASSERT_EQUAL_INT(0, bad);
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_valid_empty);

	/* Exhaustive differential verification vs Table 3-7 oracle */
	RUN_TEST(test_oracle_self_check);
	RUN_TEST(test_exhaustive_1byte);
	RUN_TEST(test_exhaustive_2byte);
	RUN_TEST(test_exhaustive_3byte);
	RUN_TEST(test_exhaustive_4byte_leads);
	RUN_TEST(test_composition_valid_concatenations);
	RUN_TEST(test_composition_differential_mutated);
	RUN_TEST(test_composition_differential_random);
	RUN_TEST(test_sanitize_examples);
	RUN_TEST(test_sanitize_stops_on_a_character_boundary);
	RUN_TEST(test_sanitize_output_is_always_safe);

	return TEST_END();
}
