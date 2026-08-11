/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_offset.c: the G0 offset space (design §11 step 2).
 *
 * bufOffset()/bufPos() define the flat byte-offset space against the
 * current row array.  Everything G0 does afterwards addresses text
 * through that space, so these three properties are the foundation:
 *
 *   1. bufTextLen() agrees with rowsToString(), byte for byte.
 *   2. bufPos(bufOffset(cx, cy)) == (cx, cy) for every in-bounds
 *      position -- the conversions are exact inverses.
 *   3. The byte at an offset is the byte rowsToString() puts there.
 *
 * Property 3 is the one that catches an off-by-one in the separator
 * accounting, which properties 1 and 2 can both survive: a length and
 * a round trip can agree with each other while both disagree with the
 * text.
 */

#include "test.h"
#include "test_harness.h"
#include "buffer.h"
#include "fileio.h"
#include "util.h"
#include <stdint.h>
#include <string.h>

void setUp(void) {
	initTestEditor();
}

void tearDown(void) {
	cleanupTestEditor();
}

/* ---- Property 1: length agrees with the flattened text ---- */

static void assertLenMatchesFlat(struct buffer *buf) {
	size_t flatlen;
	char *flat = rowsToString(buf, &flatlen);
	TEST_ASSERT_EQUAL_UINT(flatlen, bufTextLen(buf));
	free(flat);
}

void test_len_single_line(void) {
	assertLenMatchesFlat(make_test_buffer("alpha"));
}

void test_len_empty_buffer(void) {
	struct buffer *buf = make_test_buffer("");
	TEST_ASSERT_EQUAL_UINT(0, bufTextLen(buf));
	assertLenMatchesFlat(buf);
}

void test_len_multi_line(void) {
	const char *lines[] = { "alpha", "beta", "gamma" };
	assertLenMatchesFlat(make_test_buffer_lines(lines, 3));
}

/* The trailing empty row is how the current model spells "the buffer
 * ends in a newline".  Its separator must be counted, or every offset
 * past the last real row is short by one. */
void test_len_trailing_empty_row(void) {
	/* newBuffer() seeds a trailing empty row, so this is four rows:
	 * "alpha", "beta", "", "" -- the text "alpha\nbeta\n\n". */
	const char *lines[] = { "alpha", "beta", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	TEST_ASSERT_EQUAL_UINT(12, bufTextLen(buf));
	assertLenMatchesFlat(buf);
}

void test_len_embedded_blank_lines(void) {
	/* Five rows with the seeded trailing empty: "a\n\n\nb\n". */
	const char *lines[] = { "a", "", "", "b" };
	struct buffer *buf = make_test_buffer_lines(lines, 4);
	TEST_ASSERT_EQUAL_UINT(6, bufTextLen(buf));
	assertLenMatchesFlat(buf);
}

void test_len_multibyte(void) {
	const char *lines[] = { "\xc3\xa9\xc3\xa9", "\xe6\xbc\xa2" };
	assertLenMatchesFlat(make_test_buffer_lines(lines, 2));
}

/* ---- Property 2: bufOffset and bufPos are exact inverses ---- */

/* Walk every legal (cx, cy) in the buffer and assert the round trip.
 * Exhaustive rather than sampled: the buffers are small, and the
 * failure this guards against is at a boundary, not in the middle. */
static void assertRoundTripAll(struct buffer *buf) {
	for (int y = 0; y < buf->numrows; y++) {
		for (int x = 0; x <= buf->row[y].size; x++) {
			size_t off = bufOffset(buf, x, y);
			int bx, by;
			bufPos(buf, off, &bx, &by);
			if (bx != x || by != y) {
				printf("    (%d,%d) -> offset %zu -> (%d,%d)\n",
				       x, y, off, bx, by);
				TEST_ASSERT_TRUE(0);
				return;
			}
		}
	}
}

void test_roundtrip_single_line(void) {
	assertRoundTripAll(make_test_buffer("alpha beta"));
}

void test_roundtrip_multi_line(void) {
	const char *lines[] = { "alpha", "beta", "gamma" };
	assertRoundTripAll(make_test_buffer_lines(lines, 3));
}

void test_roundtrip_blank_lines(void) {
	const char *lines[] = { "a", "", "", "b", "" };
	assertRoundTripAll(make_test_buffer_lines(lines, 5));
}

void test_roundtrip_empty_buffer(void) {
	assertRoundTripAll(make_test_buffer(""));
}

void test_roundtrip_multibyte(void) {
	const char *lines[] = { "\xc3\xa9x\xc3\xa9", "\xe6\xbc\xa2\xe5\xad\x97",
			        "plain" };
	assertRoundTripAll(make_test_buffer_lines(lines, 3));
}

/* ---- Property 3: the offset space indexes the flattened text ---- */

/* For every offset, the byte bufPos() names must be the byte
 * rowsToString() puts at that offset.  End-of-row offsets name the
 * separator newline, which has no (cx, cy) byte of its own, so they
 * are checked against '\n' directly. */
void test_offset_indexes_flat_text(void) {
	const char *lines[] = { "alpha", "", "gamma", "" };
	struct buffer *buf = make_test_buffer_lines(lines, 4);
	size_t flatlen;
	char *flat = rowsToString(buf, &flatlen);

	for (size_t off = 0; off < flatlen; off++) {
		int x, y;
		bufPos(buf, off, &x, &y);
		char expect = flat[off];
		char got = (x < buf->row[y].size) ? (char)buf->row[y].chars[x]
						  : '\n';
		if (got != expect) {
			printf("    offset %zu: flat '%c' but (%d,%d) gives '%c'\n",
			       off, expect, x, y, got);
			TEST_ASSERT_TRUE(0);
			break;
		}
	}
	free(flat);
}

/* ---- Boundaries ---- */

void test_offset_start_is_zero(void) {
	const char *lines[] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	TEST_ASSERT_EQUAL_UINT(0, bufOffset(buf, 0, 0));
}

void test_offset_end_is_textlen(void) {
	const char *lines[] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	int last = buf->numrows - 1;
	TEST_ASSERT_EQUAL_UINT(bufTextLen(buf),
				 bufOffset(buf, buf->row[last].size, last));
}

/* Row starts sit one past the previous row's end: the separator
 * newline occupies the offset between them. */
void test_offset_row_start_follows_separator(void) {
	const char *lines[] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	TEST_ASSERT_EQUAL_UINT(5, bufOffset(buf, 5, 0)); /* end of row 0 */
	TEST_ASSERT_EQUAL_UINT(6, bufOffset(buf, 0, 1)); /* start of row 1 */
}

/* Out-of-range input clamps rather than reading past the row array.
 * Callers mid-migration will hold stale coordinates; clamping keeps
 * that a wrong answer rather than a crash. */
void test_offset_clamps_out_of_range(void) {
	const char *lines[] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	TEST_ASSERT_EQUAL_UINT(0, bufOffset(buf, -5, -5));
	TEST_ASSERT_EQUAL_UINT(bufTextLen(buf), bufOffset(buf, 999, 999));
}

void test_pos_clamps_past_end(void) {
	const char *lines[] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	int x, y;
	bufPos(buf, bufTextLen(buf) + 100, &x, &y);
	TEST_ASSERT_EQUAL_INT(buf->numrows - 1, y);
	TEST_ASSERT_EQUAL_INT(buf->row[buf->numrows - 1].size, x);
}

int main(void) {
	TEST_BEGIN();

	/* Property 1: length agrees with rowsToString */
	RUN_TEST(test_len_single_line);
	RUN_TEST(test_len_empty_buffer);
	RUN_TEST(test_len_multi_line);
	RUN_TEST(test_len_trailing_empty_row);
	RUN_TEST(test_len_embedded_blank_lines);
	RUN_TEST(test_len_multibyte);

	/* Property 2: exact inverses */
	RUN_TEST(test_roundtrip_single_line);
	RUN_TEST(test_roundtrip_multi_line);
	RUN_TEST(test_roundtrip_blank_lines);
	RUN_TEST(test_roundtrip_empty_buffer);
	RUN_TEST(test_roundtrip_multibyte);

	/* Property 3: indexes the flattened text */
	RUN_TEST(test_offset_indexes_flat_text);

	/* Boundaries */
	RUN_TEST(test_offset_start_is_zero);
	RUN_TEST(test_offset_end_is_textlen);
	RUN_TEST(test_offset_row_start_follows_separator);
	RUN_TEST(test_offset_clamps_out_of_range);
	RUN_TEST(test_pos_clamps_past_end);

	return TEST_END();
}
