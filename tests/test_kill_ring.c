/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_kill_ring.c
 *
 * Kill, yank and yank-pop, checked by what lands in the buffer.
 *
 *   1. Kill then yank returns the most recent kill.
 *   2. Kill, kill, yank, yank-pop returns the previous kill.
 *   3. A rectangle kill yanked back preserves its geometry.
 *   4. An empty kill is not recorded.
 *
 * The tests issue commands through processKeypress() and read the
 * buffer text, point and mark.  They do not look at how the ring is
 * stored, so the storage can change without touching them.  The one
 * exception is the last test, which clears the undo history by hand;
 * its comment says why. */

#include "test.h"
#include "test_harness.h"
#include "emil.h"
#include "buffer.h"
#include "fileio.h"
#include "keymap.h"
#include "region.h"
#include "undo.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>



void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* The whole buffer as the bytes a save would write. */
#define ASSERT_TEXT(expected, buf)                           \
	do {                                                 \
		size_t _len;                                 \
		char *_text = rowsToString((buf), &_len);    \
		TEST_ASSERT_EQUAL_STRING((expected), _text); \
		free(_text);                                 \
	} while (0)

/* Helper: set mark at (mx, my) and point at (px, py). */
static void set_region(struct buffer *buf, int mx, int my, int px, int py) {
	buf->markx = mx;
	buf->marky = my;
	buf->mark_active = 1;
	buf->cx = px;
	buf->cy = py;
}

/* Helper: set mark at (mx, my), point at (px, py), then C-w. */
static void kill_range(struct buffer *buf, int mx, int my, int px, int py) {
	set_region(buf, mx, my, px, py);
	processKeypress(CMD_KILL_REGION);
}

/* Helper: put point at the end of row 0. */
static void end_of_first_row(struct buffer *buf) {
	buf->cx = buf->row[0].size;
	buf->cy = 0;
}

/* --- 1. kill + yank → most recent kill ---------------------------- */

void test_yank_returns_most_recent_kill(void) {
	struct buffer *buf = make_test_buffer("hello world");

	/* Kill "world" (columns 6..11 on row 0). */
	kill_range(buf, 6, 0, 11, 0);
	TEST_ASSERT_EQUAL_STRING("hello ", row_str(buf, 0));

	/* Yank it back at end of line. */
	end_of_first_row(buf);
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("hello world", row_str(buf, 0));
}

/* --- 2. two kills + yank + yank-pop → previous kill --------------- */

void test_yank_pop_returns_previous_kill(void) {
	struct buffer *buf = make_test_buffer("abc def ghi");

	/* Kill "ghi" (first kill, most recent). */
	kill_range(buf, 8, 0, 11, 0);
	TEST_ASSERT_EQUAL_STRING("abc def ", row_str(buf, 0));

	/* Kill "def" (second kill, now most recent; "ghi" is older). */
	kill_range(buf, 4, 0, 7, 0);
	TEST_ASSERT_EQUAL_STRING("abc  ", row_str(buf, 0));

	/* Yank at end: most recent kill = "def". */
	end_of_first_row(buf);
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("abc  def", row_str(buf, 0));

	/* M-y: replace the just-yanked text with the previous kill "ghi". */
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT_EQUAL_STRING("abc  ghi", row_str(buf, 0));
}

/* --- 3. rectangle kill → yanked back preserves geometry ----------- */

void test_rectangle_yank_preserves_geometry(void) {
	/* Build a 3-row buffer.  A 2x2 rectangle from (col 2, row 0) to
	 * (col 4, row 1) covers:
	 *   "ABCDEF"    "AB"  — cols [2..4) of rows 0,1 → "CD" and "cd"
	 *   "abcdef"
	 *   "123456"
	 * so the killed rectangle is "CD" over "cd": two columns wide,
	 * two rows high. */
	const char *lines[] = { "ABCDEF", "abcdef", "123456" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	/* Mark at (2, 0), point at (4, 1), then C-x r k. */
	set_region(buf, 2, 0, 4, 1);
	processKeypress(CMD_KILL_RECT);

	/* After kill: columns [2..4) removed from rows 0,1. */
	ASSERT_TEXT("ABEF\nabef\n123456\n", buf);

	/* Yank the rectangle back at its original top-left.  Geometry is
	 * preserved: the original text returns. */
	buf->cx = 2;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);
	ASSERT_TEXT("ABCDEF\nabcdef\n123456\n", buf);

	/* Yanked somewhere else it is still two columns by two rows:
	 * "CD" goes into the row at point and "cd" into the row below,
	 * at the same column. */
	buf->cx = 0;
	buf->cy = 1;
	processKeypress(CMD_YANK_RECT);
	ASSERT_TEXT("ABCDEF\nCDabcdef\ncd123456\n", buf);
}

/* --- 4. empty kill is not recorded -------------------------------- */

void test_empty_kill_not_recorded(void) {
	struct buffer *buf = make_test_buffer("word text");

	/* One real kill: "word". */
	kill_range(buf, 0, 0, 4, 0);
	TEST_ASSERT_EQUAL_STRING(" text", row_str(buf, 0));

	/* C-w with point == mark is a zero-length region.  It removes
	 * nothing and must not put an empty entry on the ring. */
	kill_range(buf, 0, 0, 0, 0);
	TEST_ASSERT_EQUAL_STRING(" text", row_str(buf, 0));

	/* So the latest kill is still "word"... */
	buf->cx = 0;
	buf->cy = 0;
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("word text", row_str(buf, 0));

	/* ...and it is the only one: M-y cycles back round to it. */
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT_EQUAL_STRING("word text", row_str(buf, 0));
}

/* --- 5. rectangle yank with no rectangle kill --------------------- */

/* Regression: C-x r y is bound to yankRectangle unconditionally.
 * With an empty or plain-text kill (rect_height == 0), the geometry
 * arithmetic computed boty = cy - 1 and read row[-1] at the top of
 * the buffer (heap-buffer-overflow under ASAN). */
void test_yank_rectangle_without_rect_kill(void) {
	struct buffer *buf = make_test_buffer("hello");
	buf->cx = 0;
	buf->cy = 0;

	/* No kill at all */
	processKeypress(CMD_YANK_RECT);
	ASSERT_TEXT("hello\n", buf);

	/* Plain-text (non-rectangle) kill: M-w over "hello". */
	set_region(buf, 0, 0, 5, 0);
	processKeypress(CMD_COPY);
	buf->cx = 0;
	buf->cy = 0;
	processKeypress(CMD_YANK_RECT);
	ASSERT_TEXT("hello\n", buf);

	/* Read-only buffer with a real rectangle kill ("he", copied
	 * with C-x r M-w): no modification. */
	set_region(buf, 0, 0, 2, 0);
	processKeypress(CMD_COPY_RECT);
	buf->cx = 0;
	buf->cy = 0;
	buf->read_only = 1;
	processKeypress(CMD_YANK_RECT);
	ASSERT_TEXT("hello\n", buf);

	/* The same yank goes through once the buffer is writable, so
	 * the refusal above was the read-only check and not a missing
	 * rectangle. */
	buf->read_only = 0;
	processKeypress(CMD_YANK_RECT);
	ASSERT_TEXT("hehello\n", buf);
}

/* --- 6. C-u C-y: reverse yank — point before, mark after ---------- */

void test_reverse_yank_leaves_point_before_text(void) {
	struct buffer *buf = make_test_buffer("hello world");

	/* Kill "world". */
	kill_range(buf, 6, 0, 11, 0);
	TEST_ASSERT_EQUAL_STRING("hello ", row_str(buf, 0));

	/* C-u C-y at end of line, driven through the real dispatch so
	 * the uarg hand-off is exercised.  Any positive uarg means
	 * reverse; bare C-u arrives as 4. */
	end_of_first_row(buf);
	E.uarg = 4;
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("hello world", row_str(buf, 0));

	/* Point stayed before the yanked text; the mark position is
	 * set after it, but the mark is left inactive (yank no longer
	 * activates a region around the inserted text). */
	TEST_ASSERT_EQUAL_INT(6, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT_EQUAL_INT(11, buf->markx);
	TEST_ASSERT_EQUAL_INT(0, buf->marky);
	TEST_ASSERT_FALSE(buf->mark_active);
}

/* --- 7. M-- M-y: cycle the kill ring toward newer kills ----------- */

void test_reverse_yank_pop_cycles_forward(void) {
	struct buffer *buf = make_test_buffer("abc def ghi");

	/* Ring, oldest → newest: "ghi", "def". */
	kill_range(buf, 8, 0, 11, 0);
	kill_range(buf, 4, 0, 7, 0);
	TEST_ASSERT_EQUAL_STRING("abc  ", row_str(buf, 0));

	/* Yank ("def"), M-y (→ "ghi"), then M-- M-y returns to "def". */
	end_of_first_row(buf);
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("abc  def", row_str(buf, 0));
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT_EQUAL_STRING("abc  ghi", row_str(buf, 0));
	processKeypress(CMD_NEGATIVE_ARG);
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT_EQUAL_STRING("abc  def", row_str(buf, 0));
}

/* --- 8. designed no-ops: M-- C-y and C-u M-y ---------------------- */

void test_reverse_modifier_noop_combinations(void) {
	struct buffer *buf = make_test_buffer("stub ");

	kill_range(buf, 0, 0, 4, 0);
	TEST_ASSERT_EQUAL_STRING(" ", row_str(buf, 0));

	/* M-- C-y: nothing happens. */
	end_of_first_row(buf);
	processKeypress(CMD_NEGATIVE_ARG);
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING(" ", row_str(buf, 0));

	/* Yank, then C-u M-y: nothing happens, buffer unchanged. */
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING(" stub", row_str(buf, 0));
	processKeypress(CMD_UNIVERSAL_ARG);
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT_EQUAL_STRING(" stub", row_str(buf, 0));
}

/* --- 9. argument keys don't break the yank chain ------------------ */

void test_negative_arg_preserves_kill_ring_pos(void) {
	struct buffer *buf = make_test_buffer("abc def ghi");

	kill_range(buf, 8, 0, 11, 0);
	kill_range(buf, 4, 0, 7, 0);

	end_of_first_row(buf);

	/* C-y, then M-- as a keypress, then M-y: the M-- keystroke must
	 * not break the yank chain, or the pop reports "not after
	 * yank".  Full dispatch path. */
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("abc  def", row_str(buf, 0));
	processKeypress(CMD_NEGATIVE_ARG);
	TEST_ASSERT_EQUAL_INT(UARG_REVERSE, E.uarg);
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT_EQUAL_STRING("abc  ghi", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(0, E.uarg); /* consumed by dispatch */
}

/* --- 10. yank at end of buffer, where the yank provokes a
 * final-newline repair, still permits M-y -------------------------
 *
 * The repair chains a paired record on top of the yank's insert.
 * Locked in because it is the case the undo-stack test was suspected
 * of breaking; measured, the repair record is itself an insert, so
 * it never did.  The test documents that rather than guarding a
 * regression. */
void test_yank_pop_after_final_newline_repair(void) {
	const char *lines[] = { "hello", "second" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);

	kill_range(buf, 0, 0, 5, 0); /* kill "hello" */
	kill_range(buf, 0, 1, 6, 1); /* kill "second" */
	ASSERT_TEXT("\n\n", buf);

	/* Yank at the very end of the buffer: the text lands on the
	 * last line and the buffer still ends with a newline. */
	buf->cy = buf->numrows - 1;
	buf->cx = buf->row[buf->cy].size;
	processKeypress(CMD_YANK);
	ASSERT_TEXT("\n\nsecond\n", buf);

	/* M-y must be accepted and must cycle to the older kill. */
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT(strstr(E.statusmsg, "not a yank") == NULL);
	ASSERT_TEXT("\n\nhello\n", buf);
}

/* --- 11. M-y with no preceding yank is still refused -------------- */
void test_yank_pop_without_preceding_yank_refused(void) {
	struct buffer *buf = make_test_buffer("abc");

	kill_range(buf, 0, 0, 3, 0);
	buf->cx = 0;
	buf->cy = 0;
	ASSERT_TEXT("\n", buf);

	/* Never yanked: M-y has nothing to replace. */
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT(strstr(E.statusmsg, "not a yank") != NULL);
	ASSERT_TEXT("\n", buf);
}

/* --- 12. an intervening command breaks the yank chain ------------- */
void test_intervening_command_breaks_yank_chain(void) {
	struct buffer *buf = make_test_buffer("abc def ghi");

	kill_range(buf, 0, 0, 3, 0); /* "abc" */
	kill_range(buf, 0, 0, 4, 0); /* " def" */
	end_of_first_row(buf);

	processKeypress(CMD_YANK);
	ASSERT_TEXT(" ghi def\n", buf);

	/* Any other command ends the chain... */
	processKeypress(CMD_FORWARD_CHAR);

	/* ...so M-y is refused and the yanked text stays as it is. */
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT(strstr(E.statusmsg, "not a yank") != NULL);
	ASSERT_TEXT(" ghi def\n", buf);
}

/* --- 13. M-y with kill_ring_pos set but the undo stack cleared ----
 *
 * Reachable from a prompt: C-y dispatches through processKeypress
 * (so kill_ring_pos is set), but Up is handled by the prompt loop
 * itself, and replaceMinibufferText clears the minibuffer's undo
 * history.  M-y must refuse rather than yank a second copy on top
 * of the first. */
void test_yank_pop_refused_when_undo_cleared(void) {
	struct buffer *buf = make_test_buffer("abc def ghi");

	kill_range(buf, 0, 0, 3, 0);
	kill_range(buf, 0, 0, 4, 0);
	end_of_first_row(buf);

	processKeypress(CMD_YANK);
	TEST_ASSERT(E.kill_ring_pos >= 0);
	const char *after_yank = row_str(buf, 0);
	char snapshot[128];
	emil_strlcpy(snapshot, after_yank, sizeof(snapshot));

	/* Simulate the prompt's history browse. */
	clearUndosAndRedos(buf);

	yankPop(0);
	TEST_ASSERT(strstr(E.statusmsg, "not a yank") != NULL);
	/* Buffer untouched: no duplicated text. */
	TEST_ASSERT_EQUAL_STRING(snapshot, row_str(buf, 0));
}

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_yank_returns_most_recent_kill);
	RUN_TEST(test_yank_pop_returns_previous_kill);
	RUN_TEST(test_rectangle_yank_preserves_geometry);
	RUN_TEST(test_empty_kill_not_recorded);
	RUN_TEST(test_yank_rectangle_without_rect_kill);
	RUN_TEST(test_reverse_yank_leaves_point_before_text);
	RUN_TEST(test_reverse_yank_pop_cycles_forward);
	RUN_TEST(test_reverse_modifier_noop_combinations);
	RUN_TEST(test_negative_arg_preserves_kill_ring_pos);
	RUN_TEST(test_yank_pop_after_final_newline_repair);
	RUN_TEST(test_yank_pop_without_preceding_yank_refused);
	RUN_TEST(test_intervening_command_breaks_yank_chain);
	RUN_TEST(test_yank_pop_refused_when_undo_cleared);
	return TEST_END();
}
