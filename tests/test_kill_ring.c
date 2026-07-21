/* test_kill_ring.c
 *
 * Kill ring operations today touch three things in parallel:
 *   - E.kill (the "current" kill, used as a read cache)
 *   - E.kill_history (the ring itself)
 *   - E.kill_ring_pos (cursor within the ring for M-y)
 *
 *   1. Kill then yank returns the most recent kill.
 *   2. Kill, kill, yank, yank-pop returns the previous kill.
 *   3. A rectangle kill yanked back preserves its geometry.
 *   4. An empty kill is not recorded.
 *
 * The tests exercise killRegion/yank/yankPop directly rather than
 * key sequences — the dispatch layer is covered by test_keymap. */

#include "test.h"
#include "test_harness.h"
#include "emil.h"
#include "buffer.h"
#include "region.h"
#include "keymap.h"
#include <string.h>

extern struct config E;

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* Helper: set mark at (mx, my), point at (px, py), kill the region
 * between them.  killRegion handles normalisation internally. */
static void kill_range(struct buffer *buf, int mx, int my, int px, int py) {
	buf->markx = mx;
	buf->marky = my;
	buf->mark_active = 1;
	buf->cx = px;
	buf->cy = py;
	killRegion();
}

/* --- 1. kill + yank → most recent kill ---------------------------- */

void test_yank_returns_most_recent_kill(void) {
	struct buffer *buf = make_test_buffer("hello world");

	/* Kill "world" (columns 6..11 on row 0). */
	kill_range(buf, 6, 0, 11, 0);
	TEST_ASSERT_EQUAL_STRING("hello ", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, E.kill_history.count);

	/* Yank it back at end of line. */
	buf->cx = buf->row[0].size;
	buf->cy = 0;
	yank(0);
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
	TEST_ASSERT_EQUAL_INT(2, E.kill_history.count);

	/* Yank at end: most recent kill = "def". */
	buf->cx = buf->row[0].size;
	buf->cy = 0;
	yank(0);
	TEST_ASSERT_EQUAL_STRING("abc  def", row_str(buf, 0));

	/* M-y: replace the just-yanked text with the previous kill "ghi". */
	yankPop(0);
	TEST_ASSERT_EQUAL_STRING("abc  ghi", row_str(buf, 0));
}

/* --- 3. rectangle kill → yanked back preserves geometry ----------- */

void test_rectangle_yank_preserves_geometry(void) {
	/* Build a 3-row buffer.  A 2x2 rectangle from (col 2, row 0) to
	 * (col 4, row 1) covers:
	 *   "ABCDEF"    "AB"  — cols [2..4) of rows 0,1 → "CD" and "cd"
	 *   "abcdef"
	 *   "123456"
	 * so the rectangle kill is "CDcd", width=2, height=2. */
	const char *lines[] = { "ABCDEF", "abcdef", "123456" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	/* Enter rectangle mode.  Mark at (2, 0), point at (4, 1). */
	buf->rectangle_mode = 1;
	buf->markx = 2;
	buf->marky = 0;
	buf->mark_active = 1;
	buf->cx = 4;
	buf->cy = 1;

	killRectangle();

	/* After kill: columns [2..4) removed from rows 0,1. */
	TEST_ASSERT_EQUAL_STRING("ABEF", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("abef", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("123456", row_str(buf, 2));

	/* Kill ring entry is a rectangle with width=2, height=2. */
	struct historyEntry *last = getLastHistory(&E.kill_history);
	TEST_ASSERT_NOT_NULL(last);
	TEST_ASSERT_TRUE(last->is_rectangle);
	TEST_ASSERT_EQUAL_INT(2, last->rect_width);
	TEST_ASSERT_EQUAL_INT(2, last->rect_height);

	/* Yank the rectangle back at its original top-left.  Geometry is
	 * preserved: the original text returns. */
	buf->cx = 2;
	buf->cy = 0;
	yankRectangle();
	TEST_ASSERT_EQUAL_STRING("ABCDEF", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("abcdef", row_str(buf, 1));
}

/* --- 4. empty kill is not recorded -------------------------------- */

void test_empty_kill_not_recorded(void) {
	struct buffer *buf = make_test_buffer("text");
	(void)buf;

	int count_before = E.kill_history.count;

	/* An empty-string kill: addToKillRing returns early, ring unchanged. */
	addToKillRing("", 0, 0, 0);
	TEST_ASSERT_EQUAL_INT(count_before, E.kill_history.count);

	/* A NULL kill: same. */
	addToKillRing(NULL, 0, 0, 0);
	TEST_ASSERT_EQUAL_INT(count_before, E.kill_history.count);

	/* killRegion with point == mark is a zero-length range; deleteRange
	 * returns early and nothing is recorded. */
	buf->markx = 0;
	buf->marky = 0;
	buf->mark_active = 1;
	buf->cx = 0;
	buf->cy = 0;
	killRegion();
	TEST_ASSERT_EQUAL_INT(count_before, E.kill_history.count);
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
	yankRectangle();
	TEST_ASSERT_EQUAL_STRING("hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);

	/* Plain-text (non-rectangle) kill */
	addToKillRing("plain", 0, 0, 0);
	yankRectangle();
	TEST_ASSERT_EQUAL_STRING("hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);

	/* Read-only buffer with a real rectangle kill: no modification */
	addToKillRing("ab", 1, 2, 1);
	buf->read_only = 1;
	yankRectangle();
	TEST_ASSERT_EQUAL_STRING("hello", row_str(buf, 0));
	buf->read_only = 0;
}

/* --- runner ------------------------------------------------------- */

/* --- 6. C-u C-y: reverse yank — point before, mark after ---------- */

void test_reverse_yank_leaves_point_before_text(void) {
	struct buffer *buf = make_test_buffer("hello world");

	/* Kill "world". */
	kill_range(buf, 6, 0, 11, 0);
	TEST_ASSERT_EQUAL_STRING("hello ", row_str(buf, 0));

	/* C-u C-y at end of line, driven through the real dispatch so
	 * the uarg hand-off is exercised.  Any positive uarg means
	 * reverse; bare C-u arrives as 4. */
	buf->cx = buf->row[0].size;
	buf->cy = 0;
	E.uarg = 4;
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("hello world", row_str(buf, 0));

	/* Point stayed before the yanked text; mark sits after it. */
	TEST_ASSERT_EQUAL_INT(6, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT_EQUAL_INT(11, buf->markx);
	TEST_ASSERT_EQUAL_INT(0, buf->marky);
	TEST_ASSERT_TRUE(buf->mark_active);
}

/* --- 7. M-- M-y: cycle the kill ring toward newer kills ----------- */

void test_reverse_yank_pop_cycles_forward(void) {
	struct buffer *buf = make_test_buffer("abc def ghi");

	/* Ring, oldest → newest: "ghi", "def". */
	kill_range(buf, 8, 0, 11, 0);
	kill_range(buf, 4, 0, 7, 0);
	TEST_ASSERT_EQUAL_STRING("abc  ", row_str(buf, 0));

	/* Yank ("def"), M-y (→ "ghi"), then M-- M-y returns to "def". */
	buf->cx = buf->row[0].size;
	buf->cy = 0;
	yank(0);
	TEST_ASSERT_EQUAL_STRING("abc  def", row_str(buf, 0));
	yankPop(0);
	TEST_ASSERT_EQUAL_STRING("abc  ghi", row_str(buf, 0));
	yankPop(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("abc  def", row_str(buf, 0));
}

/* --- 8. designed no-ops: M-- C-y and C-u M-y ---------------------- */

void test_reverse_modifier_noop_combinations(void) {
	struct buffer *buf = make_test_buffer("stub ");

	kill_range(buf, 0, 0, 4, 0);
	TEST_ASSERT_EQUAL_STRING(" ", row_str(buf, 0));

	/* M-- C-y: nothing happens. */
	buf->cx = buf->row[0].size;
	buf->cy = 0;
	yank(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING(" ", row_str(buf, 0));

	/* Yank, then C-u M-y: nothing happens, buffer unchanged. */
	yank(0);
	TEST_ASSERT_EQUAL_STRING(" stub", row_str(buf, 0));
	yankPop(4);
	TEST_ASSERT_EQUAL_STRING(" stub", row_str(buf, 0));
}

/* --- 9. argument keys don't break the yank chain ------------------ */

void test_negative_arg_preserves_kill_ring_pos(void) {
	struct buffer *buf = make_test_buffer("abc def ghi");

	kill_range(buf, 8, 0, 11, 0);
	kill_range(buf, 4, 0, 7, 0);

	buf->cx = buf->row[0].size;
	buf->cy = 0;

	/* C-y, then M-- as a keypress, then M-y: the M-- keystroke must
	 * not reset E.kill_ring_pos, or the pop reports "not after
	 * yank".  Full dispatch path. */
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL_STRING("abc  def", row_str(buf, 0));
	processKeypress(CMD_NEGATIVE_ARG);
	TEST_ASSERT_EQUAL_INT(UARG_REVERSE, E.uarg);
	processKeypress(CMD_YANK_POP);
	TEST_ASSERT_EQUAL_STRING("abc  ghi", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(0, E.uarg); /* consumed by dispatch */
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
	return TEST_END();
}
