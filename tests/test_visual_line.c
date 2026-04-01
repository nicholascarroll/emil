/* test_visual_line.c — Visual line movement, start/end, kill in wrap mode. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include <stdint.h>

/* ---- Helper ---- */

static erow make_row(const char *s) {
	erow r;
	memset(&r, 0, sizeof(r));
	r.size = strlen(s);
	r.chars = (uint8_t *)s;
	r.cached_width = -1;
	return r;
}

/* ---- displayColumnToByteOffset tests ---- */

void test_dcbo_simple_ascii(void) {
	erow row = make_row("Hello, world! This is a long line.");
	/* On a 20-col screen, sub-line 0 starts at byte 0 */
	int b = displayColumnToByteOffset(&row, 20, 0, 0);
	TEST_ASSERT_EQUAL_INT(0, b);
	b = displayColumnToByteOffset(&row, 20, 0, 5);
	TEST_ASSERT_EQUAL_INT(5, b);
}

void test_dcbo_second_subline(void) {
	/* 40 'a' chars on a 20-col screen = 2 sub-lines */
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	erow row = make_row(buf);
	/* Sub-line 1 starts at byte 20 */
	int b = displayColumnToByteOffset(&row, 20, 1, 0);
	TEST_ASSERT_EQUAL_INT(20, b);
	b = displayColumnToByteOffset(&row, 20, 1, 5);
	TEST_ASSERT_EQUAL_INT(25, b);
}

void test_dcbo_clamp_to_subline_end(void) {
	/* "Hello" on 20-col screen: only 1 sub-line, 5 chars */
	erow row = make_row("Hello");
	/* target_col=99 should clamp to byte 5 (end of row) */
	int b = displayColumnToByteOffset(&row, 20, 0, 99);
	TEST_ASSERT_EQUAL_INT(5, b);
}

void test_dcbo_nonexistent_subline(void) {
	erow row = make_row("Hello");
	/* Sub-line 1 doesn't exist on a short line */
	int b = displayColumnToByteOffset(&row, 20, 1, 0);
	TEST_ASSERT_EQUAL_INT(5, b); /* clamped to row->size */
}

void test_dcbo_empty_row(void) {
	erow row = make_row("");
	int b = displayColumnToByteOffset(&row, 20, 0, 0);
	TEST_ASSERT_EQUAL_INT(0, b);
}

void test_dcbo_with_tab(void) {
	erow row = make_row("\tHello");
	/* Tab at col 0 expands to col 8. target_col=0 -> byte 0 (the tab) */
	int b = displayColumnToByteOffset(&row, 80, 0, 0);
	TEST_ASSERT_EQUAL_INT(0, b);
	/* target_col=8 -> byte 1 ('H') */
	b = displayColumnToByteOffset(&row, 80, 0, 8);
	TEST_ASSERT_EQUAL_INT(1, b);
}

/* ---- sublineBounds tests ---- */

void test_subline_bounds_single_line(void) {
	erow row = make_row("Hello");
	int sb, eb;
	int ok = sublineBounds(&row, 80, 0, &sb, &eb);
	TEST_ASSERT_TRUE(ok);
	TEST_ASSERT_EQUAL_INT(0, sb);
	TEST_ASSERT_EQUAL_INT(5, eb); /* row->size for last subline */
}

void test_subline_bounds_wrapped(void) {
	/* 40 'a' chars, 20-col screen => 2 sub-lines */
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	erow row = make_row(buf);
	int sb, eb;

	/* Sub-line 0: bytes 0..19 */
	int ok = sublineBounds(&row, 20, 0, &sb, &eb);
	TEST_ASSERT_TRUE(ok);
	TEST_ASSERT_EQUAL_INT(0, sb);
	TEST_ASSERT_EQUAL_INT(20, eb);

	/* Sub-line 1: bytes 20..39 */
	ok = sublineBounds(&row, 20, 1, &sb, &eb);
	TEST_ASSERT_TRUE(ok);
	TEST_ASSERT_EQUAL_INT(20, sb);
	TEST_ASSERT_EQUAL_INT(40, eb);
}

void test_subline_bounds_nonexistent(void) {
	erow row = make_row("Hello");
	int sb, eb;
	int ok = sublineBounds(&row, 80, 1, &sb, &eb);
	TEST_ASSERT_FALSE(ok);
}

/* ---- moveVisualRow (via moveCursor) integration ---- */

void test_visual_move_down_within_row(void) {
	/* 40 'a' chars on 20-col screen. Cursor at byte 5, sub-line 0. */
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 5;
	b->cy = 0;

	moveCursor(KEY_ARROW_DOWN, 1);
	/* Should move to sub-line 1, col 5 => byte 25 */
	TEST_ASSERT_EQUAL_INT(0, b->cy); /* same logical row */
	TEST_ASSERT_EQUAL_INT(25, b->cx);
	destroyBuffer(b);
}

void test_visual_move_up_within_row(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 25; /* sub-line 1, col 5 */
	b->cy = 0;

	moveCursor(KEY_ARROW_UP, 1);
	/* Should move to sub-line 0, col 5 => byte 5 */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(5, b->cx);
	destroyBuffer(b);
}

void test_visual_move_down_crosses_row(void) {
	/* Two rows: row 0 is short, row 1 has content */
	const char *lines[] = { "Hello", "World" };
	struct buffer *b = make_test_buffer_lines(lines, 2);
	E.screencols = 80;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 3;
	b->cy = 0;

	moveCursor(KEY_ARROW_DOWN, 1);
	TEST_ASSERT_EQUAL_INT(1, b->cy);
	TEST_ASSERT_EQUAL_INT(3, b->cx);
	destroyBuffer(b);
}

void test_visual_move_up_crosses_row(void) {
	/* Row 0: 40 'a' on 20-col = 2 sublines. Row 1: "Hello" */
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	const char *lines[] = { buf, "Hello" };
	struct buffer *b = make_test_buffer_lines(lines, 2);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cy = 1;
	b->cx = 3; /* col 3 on row 1 */

	moveCursor(KEY_ARROW_UP, 1);
	/* Should go to row 0, last sub-line (1), col 3 => byte 23 */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(23, b->cx);
	destroyBuffer(b);
}

void test_visual_move_down_at_buffer_end(void) {
	struct buffer *b = make_test_buffer("Hello");
	E.screencols = 80;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 3;
	b->cy = 0;

	moveCursor(KEY_ARROW_DOWN, 1);
	/* Should move to virtual line past EOF */
	TEST_ASSERT_EQUAL_INT(1, b->cy);
	TEST_ASSERT_EQUAL_INT(0, b->cx);
	destroyBuffer(b);
}

void test_visual_move_up_at_buffer_start(void) {
	struct buffer *b = make_test_buffer("Hello");
	E.screencols = 80;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 3;
	b->cy = 0;

	moveCursor(KEY_ARROW_UP, 1);
	/* Should stay put */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(3, b->cx);
	destroyBuffer(b);
}

/* ---- C-a / C-e visual line tests ---- */

void test_beginning_of_visual_line(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 25; /* sub-line 1, col 5 */
	b->cy = 0;

	beginningOfLine();
	/* Should go to start of sub-line 1 = byte 20 */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(20, b->cx);
	destroyBuffer(b);
}

void test_beginning_of_visual_line_first_subline(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 5; /* sub-line 0 */
	b->cy = 0;

	beginningOfLine();
	TEST_ASSERT_EQUAL_INT(0, b->cx); /* byte 0 */
	destroyBuffer(b);
}

void test_end_of_visual_line(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 5; /* sub-line 0, col 5 */
	b->cy = 0;

	endOfLine(0);
	/* Should go to last char of sub-line 0 = byte 19 (last 'a'
	 * before the sub-line break at byte 20) */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(19, b->cx);
	destroyBuffer(b);
}

void test_end_of_visual_line_last_subline(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 25; /* sub-line 1 */
	b->cy = 0;

	endOfLine(0);
	/* Should go to end of sub-line 1 = byte 40 = row->size */
	TEST_ASSERT_EQUAL_INT(0, b->cy);
	TEST_ASSERT_EQUAL_INT(40, b->cx);
	destroyBuffer(b);
}

/* ---- C-a / C-e without wrap (regression) ---- */

void test_beginning_of_line_no_wrap(void) {
	struct buffer *b = make_test_buffer("Hello, world!");
	E.screencols = 80;
	b->word_wrap = 0;
	b->cx = 7;

	beginningOfLine();
	TEST_ASSERT_EQUAL_INT(0, b->cx);
	destroyBuffer(b);
}

void test_end_of_line_no_wrap(void) {
	struct buffer *b = make_test_buffer("Hello, world!");
	E.screencols = 80;
	b->word_wrap = 0;
	b->cx = 0;

	endOfLine(0);
	TEST_ASSERT_EQUAL_INT(13, b->cx);
	destroyBuffer(b);
}

/* ---- C-k visual line kill ---- */

void test_kill_visual_line_mid_subline(void) {
	char buf[41];
	memset(buf, 'a', 40);
	buf[40] = '\0';
	struct buffer *b = make_test_buffer(buf);
	E.screencols = 20;
	E.windows[0]->height = 24;
	b->word_wrap = 1;
	b->cx = 5; /* sub-line 0, col 5 */
	b->cy = 0;

	killLine(0);
	/* Should kill bytes 5..19 (15 chars from sub-line 0)
	 * Remaining: 5 'a' + 20 'a' from sub-line 1 = 25 'a' */
	TEST_ASSERT_EQUAL_INT(25, b->row[0].size);
	TEST_ASSERT_EQUAL_INT(5, b->cx);
	destroyBuffer(b);
}

void test_kill_line_no_wrap(void) {
	struct buffer *b = make_test_buffer("Hello, world!");
	E.screencols = 80;
	b->word_wrap = 0;
	b->cx = 5;

	killLine(0);
	/* Should kill from byte 5 to end of line */
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(b, 0));
	destroyBuffer(b);
}

/* ---- Runner ---- */

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

int main(void) {
	TEST_BEGIN();

	/* displayColumnToByteOffset */
	RUN_TEST(test_dcbo_simple_ascii);
	RUN_TEST(test_dcbo_second_subline);
	RUN_TEST(test_dcbo_clamp_to_subline_end);
	RUN_TEST(test_dcbo_nonexistent_subline);
	RUN_TEST(test_dcbo_empty_row);
	RUN_TEST(test_dcbo_with_tab);

	/* sublineBounds */
	RUN_TEST(test_subline_bounds_single_line);
	RUN_TEST(test_subline_bounds_wrapped);
	RUN_TEST(test_subline_bounds_nonexistent);

	/* Visual row movement */
	RUN_TEST(test_visual_move_down_within_row);
	RUN_TEST(test_visual_move_up_within_row);
	RUN_TEST(test_visual_move_down_crosses_row);
	RUN_TEST(test_visual_move_up_crosses_row);
	RUN_TEST(test_visual_move_down_at_buffer_end);
	RUN_TEST(test_visual_move_up_at_buffer_start);

	/* C-a / C-e visual line */
	RUN_TEST(test_beginning_of_visual_line);
	RUN_TEST(test_beginning_of_visual_line_first_subline);
	RUN_TEST(test_end_of_visual_line);
	RUN_TEST(test_end_of_visual_line_last_subline);
	RUN_TEST(test_beginning_of_line_no_wrap);
	RUN_TEST(test_end_of_line_no_wrap);

	/* C-k visual line kill */
	RUN_TEST(test_kill_visual_line_mid_subline);
	RUN_TEST(test_kill_line_no_wrap);

	return TEST_END();
}
