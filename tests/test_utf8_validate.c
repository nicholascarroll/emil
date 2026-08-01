/* test_utf8_validate.c: Tests for the utf8_validate() utility. */

#include "test.h"
#include "test_harness.h"
#include "unicode.h"
#include <stdint.h>

/* --- Valid sequences --- */

void test_valid_ascii(void) {
	TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)"hello", 5));
	TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)"A", 1));
	TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)" ", 1));
	TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)"~", 1));
}

void test_valid_2byte(void) {
	/* ¢ = C2 A2 */
	uint8_t s[] = { 0xC2, 0xA2 };
	TEST_ASSERT_TRUE(utf8_validate(s, 2));
	/* ß = C3 9F */
	uint8_t s2[] = { 0xC3, 0x9F };
	TEST_ASSERT_TRUE(utf8_validate(s2, 2));
}

void test_valid_3byte(void) {
	/* € = E2 82 AC */
	uint8_t s[] = { 0xE2, 0x82, 0xAC };
	TEST_ASSERT_TRUE(utf8_validate(s, 3));
	/* 한 = ED 95 9C */
	uint8_t s2[] = { 0xED, 0x95, 0x9C };
	TEST_ASSERT_TRUE(utf8_validate(s2, 3));
}

void test_valid_4byte(void) {
	/* 😇 = F0 9F 98 87 */
	uint8_t s[] = { 0xF0, 0x9F, 0x98, 0x87 };
	TEST_ASSERT_TRUE(utf8_validate(s, 4));
	/* 𐍈 = F0 90 8D 88 */
	uint8_t s2[] = { 0xF0, 0x90, 0x8D, 0x88 };
	TEST_ASSERT_TRUE(utf8_validate(s2, 4));
}

void test_valid_mixed(void) {
	/* "A¢€😇" */
	uint8_t s[] = { 'A',  0xC2, 0xA2, 0xE2, 0x82,
			0xAC, 0xF0, 0x9F, 0x98, 0x87 };
	TEST_ASSERT_TRUE(utf8_validate(s, 10));
}

void test_valid_empty(void) {
	uint8_t s[] = { 0 };
	/* len=0 is trivially valid */
	TEST_ASSERT_TRUE(utf8_validate(s, 0));
}

/* --- Invalid: null byte --- */

void test_null_byte(void) {
	uint8_t s[] = { 'A', 0x00, 'B' };
	TEST_ASSERT_FALSE(utf8_validate(s, 3));
}

/* --- Invalid: overlong encodings --- */

void test_overlong_2byte(void) {
	/* C0 80 = overlong NUL */
	uint8_t s[] = { 0xC0, 0x80 };
	TEST_ASSERT_FALSE(utf8_validate(s, 2));
	/* C1 BF = overlong U+007F */
	uint8_t s2[] = { 0xC1, 0xBF };
	TEST_ASSERT_FALSE(utf8_validate(s2, 2));
}

void test_overlong_3byte(void) {
	/* E0 80 80 = overlong NUL */
	uint8_t s[] = { 0xE0, 0x80, 0x80 };
	TEST_ASSERT_FALSE(utf8_validate(s, 3));
	/* E0 9F BF = overlong U+07FF */
	uint8_t s2[] = { 0xE0, 0x9F, 0xBF };
	TEST_ASSERT_FALSE(utf8_validate(s2, 3));
}

void test_overlong_4byte(void) {
	/* F0 80 80 80 = overlong NUL */
	uint8_t s[] = { 0xF0, 0x80, 0x80, 0x80 };
	TEST_ASSERT_FALSE(utf8_validate(s, 4));
	/* F0 8F BF BF = overlong U+FFFF */
	uint8_t s2[] = { 0xF0, 0x8F, 0xBF, 0xBF };
	TEST_ASSERT_FALSE(utf8_validate(s2, 4));
}

/* --- Invalid: surrogates --- */

void test_surrogate_halves(void) {
	/* ED A0 80 = U+D800 (high surrogate) */
	uint8_t s[] = { 0xED, 0xA0, 0x80 };
	TEST_ASSERT_FALSE(utf8_validate(s, 3));
	/* ED BF BF = U+DFFF (low surrogate) */
	uint8_t s2[] = { 0xED, 0xBF, 0xBF };
	TEST_ASSERT_FALSE(utf8_validate(s2, 3));
}

/* --- Invalid: above U+10FFFF --- */

void test_above_max(void) {
	/* F4 90 80 80 = U+110000 */
	uint8_t s[] = { 0xF4, 0x90, 0x80, 0x80 };
	TEST_ASSERT_FALSE(utf8_validate(s, 4));
	/* F5 80 80 80 = lead byte > F4 */
	uint8_t s2[] = { 0xF5, 0x80, 0x80, 0x80 };
	TEST_ASSERT_FALSE(utf8_validate(s2, 4));
}

/* --- Invalid: bad continuation bytes --- */

void test_bad_continuation(void) {
	/* 2-byte with bad cont */
	uint8_t s[] = { 0xC2, 0x00 };
	TEST_ASSERT_FALSE(utf8_validate(s, 2));
	uint8_t s2[] = { 0xC2, 0xC0 };
	TEST_ASSERT_FALSE(utf8_validate(s2, 2));
	/* 3-byte with bad 2nd cont */
	uint8_t s3[] = { 0xE2, 0x82, 0x00 };
	TEST_ASSERT_FALSE(utf8_validate(s3, 3));
}

/* --- Invalid: truncated sequences --- */

void test_truncated(void) {
	/* 2-byte truncated */
	uint8_t s[] = { 0xC2 };
	TEST_ASSERT_FALSE(utf8_validate(s, 1));
	/* 3-byte truncated after 2 */
	uint8_t s2[] = { 0xE2, 0x82 };
	TEST_ASSERT_FALSE(utf8_validate(s2, 2));
	/* 4-byte truncated after 3 */
	uint8_t s3[] = { 0xF0, 0x9F, 0x98 };
	TEST_ASSERT_FALSE(utf8_validate(s3, 3));
}

/* --- Invalid: bare continuation byte as lead --- */

void test_bare_continuation(void) {
	uint8_t s[] = { 0x80 };
	TEST_ASSERT_FALSE(utf8_validate(s, 1));
	uint8_t s2[] = { 0xBF };
	TEST_ASSERT_FALSE(utf8_validate(s2, 1));
}

void setUp(void) {
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ----------------------------------------------------------------
 * Exhaustive differential verification
 *
 * The hand-picked cases above cover the known corner classes.  The
 * tests below go further: they compare utf8_validate() against an
 * independent oracle over the ENTIRE sequence space, so its
 * correctness rests on enumeration rather than on having thought of
 * every corner.
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

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_valid_ascii);
	RUN_TEST(test_valid_2byte);
	RUN_TEST(test_valid_3byte);
	RUN_TEST(test_valid_4byte);
	RUN_TEST(test_valid_mixed);
	RUN_TEST(test_valid_empty);
	RUN_TEST(test_null_byte);
	RUN_TEST(test_overlong_2byte);
	RUN_TEST(test_overlong_3byte);
	RUN_TEST(test_overlong_4byte);
	RUN_TEST(test_surrogate_halves);
	RUN_TEST(test_above_max);
	RUN_TEST(test_bad_continuation);
	RUN_TEST(test_truncated);
	RUN_TEST(test_bare_continuation);

	/* Exhaustive differential verification vs Table 3-7 oracle */
	RUN_TEST(test_oracle_self_check);
	RUN_TEST(test_exhaustive_1byte);
	RUN_TEST(test_exhaustive_2byte);
	RUN_TEST(test_exhaustive_3byte);
	RUN_TEST(test_exhaustive_4byte_leads);
	RUN_TEST(test_composition_valid_concatenations);
	RUN_TEST(test_composition_differential_mutated);
	RUN_TEST(test_composition_differential_random);

	return TEST_END();
}
