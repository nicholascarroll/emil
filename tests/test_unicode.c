/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_unicode.c: UTF-8 classification and validation. */

#include "test.h"
#include "test_harness.h"
#include "unicode.h"
#include <stdint.h>
#include <locale.h>
void test_utf8_continuation(void) {
	TEST_ASSERT_TRUE(utf8_isCont(0x80));
	TEST_ASSERT_TRUE(utf8_isCont(0xBF));
	TEST_ASSERT_FALSE(utf8_isCont('A'));
	TEST_ASSERT_FALSE(utf8_isCont(0xC0));
}

void test_utf8_char_types(void) {
	/* 2-byte start bytes */
	TEST_ASSERT_TRUE(utf8_is2Char(0xC2));
	TEST_ASSERT_TRUE(utf8_is2Char(0xDF));
	TEST_ASSERT_FALSE(utf8_is2Char(0xC1));
	TEST_ASSERT_FALSE(utf8_is2Char(0xE0));

	/* 3-byte start bytes */
	TEST_ASSERT_TRUE(utf8_is3Char(0xE0));
	TEST_ASSERT_TRUE(utf8_is3Char(0xEF));
	TEST_ASSERT_FALSE(utf8_is3Char(0xDF));
	TEST_ASSERT_FALSE(utf8_is3Char(0xF0));

	/* 4-byte start bytes */
	TEST_ASSERT_TRUE(utf8_is4Char(0xF0));
	TEST_ASSERT_TRUE(utf8_is4Char(0xF4));
	TEST_ASSERT_FALSE(utf8_is4Char(0xF5));
	TEST_ASSERT_FALSE(utf8_is4Char(0xEF));
}

void test_control_chars(void) {
	TEST_ASSERT_FALSE(ISCTRL('\0'));
	TEST_ASSERT_TRUE(ISCTRL('\n'));
	TEST_ASSERT_TRUE(ISCTRL('\r'));
	TEST_ASSERT_TRUE(ISCTRL('\t'));
	TEST_ASSERT_TRUE(ISCTRL(0x7f));
	TEST_ASSERT_FALSE(ISCTRL(' '));
	TEST_ASSERT_FALSE(ISCTRL('A'));
}

void test_invalid_lead_bytes(void) {
	TEST_ASSERT_FALSE(utf8_is4Char(0xF5));
	TEST_ASSERT_FALSE(utf8_is4Char(0xFE));
	TEST_ASSERT_FALSE(utf8_is4Char(0xFF));
	TEST_ASSERT_FALSE(utf8_is2Char(0xFE));
	TEST_ASSERT_FALSE(utf8_is3Char(0xFF));
}

void test_nbytes_all_ranges(void) {
	TEST_ASSERT_EQUAL_INT(1, utf8_nBytes(0x00));
	TEST_ASSERT_EQUAL_INT(1, utf8_nBytes(0x7F));
	TEST_ASSERT_EQUAL_INT(2, utf8_nBytes(0xC2));
	TEST_ASSERT_EQUAL_INT(2, utf8_nBytes(0xDF));
	TEST_ASSERT_EQUAL_INT(3, utf8_nBytes(0xE0));
	TEST_ASSERT_EQUAL_INT(3, utf8_nBytes(0xEF));
	TEST_ASSERT_EQUAL_INT(4, utf8_nBytes(0xF0));
	TEST_ASSERT_EQUAL_INT(4, utf8_nBytes(0xF4));
}

void setUp(void) {
}
void tearDown(void) {
	cleanupTestEditor();
}


/* ---- Character-boundary snapping ---- */

/* utf8_snapToBoundary is the single implementation of "a byte offset is
 * a legal cursor position only at the start of a character or at end of
 * line".  Before it existed, the rule was open-coded at each site that
 * moved a cursor, and the viewport clamp carried a byte offset from one
 * row to another with only a byte-length check -- so scrolling onto a
 * row of multibyte text left the cursor inside a character. */
void test_snap_boundary_forward(void) {
	/* U+65E5 U+672C U+8A9E: three 3-byte characters, 9 bytes. */
	const uint8_t r[] = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
	int n = 9;
	/* Already on a boundary: unchanged. */
	TEST_ASSERT_EQUAL_INT(0, utf8_snapToBoundary(r, n, 0, +1));
	TEST_ASSERT_EQUAL_INT(3, utf8_snapToBoundary(r, n, 3, +1));
	TEST_ASSERT_EQUAL_INT(6, utf8_snapToBoundary(r, n, 6, +1));
	/* Inside a character: forward to the next boundary. */
	TEST_ASSERT_EQUAL_INT(3, utf8_snapToBoundary(r, n, 1, +1));
	TEST_ASSERT_EQUAL_INT(3, utf8_snapToBoundary(r, n, 2, +1));
	TEST_ASSERT_EQUAL_INT(9, utf8_snapToBoundary(r, n, 7, +1));
}

void test_snap_boundary_backward(void) {
	const uint8_t r[] = "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e";
	int n = 9;
	TEST_ASSERT_EQUAL_INT(0, utf8_snapToBoundary(r, n, 1, -1));
	TEST_ASSERT_EQUAL_INT(0, utf8_snapToBoundary(r, n, 2, -1));
	TEST_ASSERT_EQUAL_INT(6, utf8_snapToBoundary(r, n, 7, -1));
	TEST_ASSERT_EQUAL_INT(6, utf8_snapToBoundary(r, n, 8, -1));
}

/* End of line is a boundary and must survive untouched.  It is also
 * where a naive backward scan would read chars[size]. */
void test_snap_boundary_end_of_line(void) {
	const uint8_t r[] = "\xe6\x97\xa5";
	TEST_ASSERT_EQUAL_INT(3, utf8_snapToBoundary(r, 3, 3, +1));
	TEST_ASSERT_EQUAL_INT(3, utf8_snapToBoundary(r, 3, 3, -1));
	/* Out of range in either direction clamps to a boundary. */
	TEST_ASSERT_EQUAL_INT(3, utf8_snapToBoundary(r, 3, 99, +1));
	TEST_ASSERT_EQUAL_INT(0, utf8_snapToBoundary(r, 3, -5, +1));
	/* An empty row has exactly one position. */
	TEST_ASSERT_EQUAL_INT(0, utf8_snapToBoundary(r, 0, 0, +1));
}

/* charAdvance is THE width rule (#117 R1).  These assertions freeze
 * it.  A change here is a change to how every subsystem measures
 * text, and must be deliberate. */
void test_char_advance_rule(void) {
	int nb;

	/* Plain ASCII: 1 column, 1 byte, x-independent. */
	TEST_ASSERT_EQUAL_INT(1, charAdvance((const uint8_t *)"A", 0, 0, &nb));
	TEST_ASSERT_EQUAL_INT(1, nb);
	TEST_ASSERT_EQUAL_INT(1, charAdvance((const uint8_t *)"A", 0, 37, &nb));

	/* Tab: distance to the next tab stop, so x matters. */
	TEST_ASSERT_EQUAL_INT(8, charAdvance((const uint8_t *)"\t", 0, 0, &nb));
	TEST_ASSERT_EQUAL_INT(1, nb);
	TEST_ASSERT_EQUAL_INT(7, charAdvance((const uint8_t *)"\t", 0, 1, &nb));
	TEST_ASSERT_EQUAL_INT(1, charAdvance((const uint8_t *)"\t", 0, 7, &nb));
	TEST_ASSERT_EQUAL_INT(8, charAdvance((const uint8_t *)"\t", 0, 8, &nb));

	/* Controls display as ^X: 2 columns.  This includes DEL (^?)
	 * and — the DEF-5 regression — NUL (^@).  One copy of the rule
	 * used to answer 1 for NUL while its neighbours answered 2. */
	TEST_ASSERT_EQUAL_INT(2, charAdvance((const uint8_t *)"\x01", 0, 0,
					     &nb));
	TEST_ASSERT_EQUAL_INT(2, charAdvance((const uint8_t *)"\x7f", 0, 0,
					     &nb));
	TEST_ASSERT_EQUAL_INT(2, charAdvance((const uint8_t *)"\x00", 0, 0,
					     &nb));
	TEST_ASSERT_EQUAL_INT(1, nb);

	/* CJK: 2 columns, 3 bytes (U+65E5 日). */
	TEST_ASSERT_EQUAL_INT(2, charAdvance((const uint8_t *)"\xe6\x97\xa5",
					     0, 0, &nb));
	TEST_ASSERT_EQUAL_INT(3, nb);

	/* Combining mark: 0 columns, 2 bytes (U+0301). */
	TEST_ASSERT_EQUAL_INT(0, charAdvance((const uint8_t *)"\xcc\x81", 0,
					     0, &nb));
	TEST_ASSERT_EQUAL_INT(2, nb);

	/* nbytes may be NULL. */
	TEST_ASSERT_EQUAL_INT(1, charAdvance((const uint8_t *)"A", 0, 0,
					     NULL));
}

/* The DEF-5 soak, promoted from the #117 report's methodology: the
 * rule must give ONE answer per byte.  nextScreenX is the historical
 * interface; a divergence between it and charAdvance means the
 * wrapper regressed. */
void test_char_advance_agrees_with_next_screen_x(void) {
	for (int c = 0; c < 128; c++) {
		for (int x = 0; x < 40; x++) {
			uint8_t s[2] = { (uint8_t)c, 0 };
			int nb;
			int adv = charAdvance(s, 0, x, &nb);
			int idx = 0;
			int nsx = nextScreenX(s, &idx, x);
			TEST_ASSERT_EQUAL_INT(adv, nsx - x);
			TEST_ASSERT_EQUAL_INT(nb - 1, idx);
		}
	}
}

void test_utf8_cols_to_bytes(void) {
	int used;

	/* ASCII: bytes == columns. */
	const uint8_t *ascii = (const uint8_t *)"hello world";
	TEST_ASSERT_EQUAL_INT(5, utf8ColsToBytes(ascii, 0, 11, 5, &used));
	TEST_ASSERT_EQUAL_INT(5, used);
	TEST_ASSERT_EQUAL_INT(11, utf8ColsToBytes(ascii, 0, 11, 99, &used));
	TEST_ASSERT_EQUAL_INT(11, used);

	/* From a nonzero offset. */
	TEST_ASSERT_EQUAL_INT(9, utf8ColsToBytes(ascii, 6, 5, 3, &used));
	TEST_ASSERT_EQUAL_INT(3, used);

	/* CJK straddle: 3 chars of 日 (9 bytes, 6 cols).  A budget of 5
	 * takes two whole characters and comes up one column short. */
	const uint8_t *cjk =
		(const uint8_t *)"\xe6\x97\xa5\xe6\x97\xa5\xe6\x97\xa5";
	TEST_ASSERT_EQUAL_INT(6, utf8ColsToBytes(cjk, 0, 9, 5, &used));
	TEST_ASSERT_EQUAL_INT(4, used);

	/* Honest zero progress: first char wider than the budget. */
	TEST_ASSERT_EQUAL_INT(0, utf8ColsToBytes(cjk, 0, 9, 1, &used));
	TEST_ASSERT_EQUAL_INT(0, used);

	/* Combining marks are 0 columns and ride along with the budget
	 * exhausted: "e" + U+0301 + "x" with 1 column takes e and the
	 * mark but not x. */
	const uint8_t *comb = (const uint8_t *)"e\xcc\x81x";
	TEST_ASSERT_EQUAL_INT(3, utf8ColsToBytes(comb, 0, 4, 1, &used));
	TEST_ASSERT_EQUAL_INT(1, used);

	/* Zero-length input. */
	TEST_ASSERT_EQUAL_INT(0, utf8ColsToBytes(ascii, 0, 0, 5, &used));
	TEST_ASSERT_EQUAL_INT(0, used);
}

/* utf8WidthN / utf8DropToFit are the shared left-truncation walk.
 * Both leftTruncate (buffer.c) and statusLeft (display.c) truncate a
 * display_name; the #117 report found them disagreeing, and sharing
 * only the per-character rule was not enough — the walk itself has to
 * be one function. */
void test_utf8_width_n(void) {
	TEST_ASSERT_EQUAL_INT(5, utf8WidthN((const uint8_t *)"hello", 5));
	TEST_ASSERT_EQUAL_INT(0, utf8WidthN((const uint8_t *)"", 0));
	/* 3 CJK = 6 columns, 9 bytes. */
	TEST_ASSERT_EQUAL_INT(6, utf8WidthN((const uint8_t *)
					    "\xE8\xAF\xAD\xE8\xAF\xAD"
					    "\xE8\xAF\xAD", 9));
	/* A tab is priced by tab stop, not as a 2-column control —
	 * this is where it differs from stringWidth. */
	TEST_ASSERT_EQUAL_INT(8, utf8WidthN((const uint8_t *)"\t", 1));
	/* 'a' then a tab lands on the SAME stop: 1 + 7 = 8, not 9.
	 * Charging a flat width per tab is what tab-stop pricing
	 * exists to avoid. */
	TEST_ASSERT_EQUAL_INT(8, utf8WidthN((const uint8_t *)"a\t", 2));
	TEST_ASSERT_EQUAL_INT(16, utf8WidthN((const uint8_t *)"a\tb\t", 4));
}

void test_utf8_drop_to_fit(void) {
	/* ASCII: drop exactly the overflow. */
	const uint8_t *a = (const uint8_t *)"abcdefghij"; /* 10 cols */
	TEST_ASSERT_EQUAL_INT(0, utf8DropToFit(a, 10, 10));
	TEST_ASSERT_EQUAL_INT(5, utf8DropToFit(a, 10, 5));
	TEST_ASSERT_EQUAL_INT(10, utf8DropToFit(a, 10, 0));

	/* CJK: drops whole characters, so an odd budget leaves the
	 * result one column under rather than splitting. */
	const uint8_t *c = (const uint8_t *)"\xE8\xAF\xAD\xE8\xAF\xAD"
					    "\xE8\xAF\xAD"; /* 6 cols */
	TEST_ASSERT_EQUAL_INT(0, utf8DropToFit(c, 9, 6));
	TEST_ASSERT_EQUAL_INT(3, utf8DropToFit(c, 9, 4));
	TEST_ASSERT_EQUAL_INT(3, utf8DropToFit(c, 9, 5)); /* whole chars */
	TEST_ASSERT_EQUAL_INT(6, utf8DropToFit(c, 9, 2));
	TEST_ASSERT_EQUAL_INT(9, utf8DropToFit(c, 9, 1)); /* nothing fits */

	/* The offset always lands on a character boundary. */
	for (int b = 0; b <= 7; b++) {
		int off = utf8DropToFit(c, 9, b);
		TEST_ASSERT_EQUAL_INT(0, off % 3);
	}
}

int main(void) {
	TEST_BEGIN();
	/* charAdvance routes multibyte widths through wcwidth, which
	 * needs an LC_CTYPE under which it is meaningful (§1.3). */
	setlocale(LC_CTYPE, "C.UTF-8");
	RUN_TEST(test_utf8_continuation);
	RUN_TEST(test_utf8_char_types);
	RUN_TEST(test_control_chars);
	RUN_TEST(test_invalid_lead_bytes);
	RUN_TEST(test_nbytes_all_ranges);
	RUN_TEST(test_snap_boundary_forward);
	RUN_TEST(test_snap_boundary_backward);
	RUN_TEST(test_snap_boundary_end_of_line);
	RUN_TEST(test_char_advance_rule);
	RUN_TEST(test_char_advance_agrees_with_next_screen_x);
	RUN_TEST(test_utf8_cols_to_bytes);
	RUN_TEST(test_utf8_width_n);
	RUN_TEST(test_utf8_drop_to_fit);
	return TEST_END();
}
