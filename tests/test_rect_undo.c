/* test_rect_undo.c — Rectangle operation undo/redo tests.
 *
 * Tests that every rectangle operation (copy, kill, yank, string-replace)
 * can be undone to restore the exact original buffer content, and that
 * redo re-applies correctly.  All tests are non-interactive: they set up
 * buffer + mark state programmatically and call the region functions
 * directly. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include "region.h"
#include <stdint.h>
#include <string.h>

/* ----------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------- */

/* Snapshot all row content so we can compare after undo. */
static char **snapshot_buffer(struct buffer *buf, int *nrows) {
	*nrows = buf->numrows;
	char **snap = calloc(buf->numrows, sizeof(char *));
	for (int i = 0; i < buf->numrows; i++)
		snap[i] = strdup((char *)buf->row[i].chars);
	return snap;
}

static void assert_buffer_matches(struct buffer *buf, char **snap,
				  int nrows, const char *label) {
	if (buf->numrows != nrows) {
		printf("  FAIL (%s): row count %d vs expected %d\n", label,
		       buf->numrows, nrows);
		_current_test_failed = 1;
		return;
	}
	for (int i = 0; i < nrows; i++) {
		if (strcmp((char *)buf->row[i].chars, snap[i]) != 0) {
			printf("  FAIL (%s): row %d: \"%s\" vs expected \"%s\"\n",
			       label, i, (char *)buf->row[i].chars, snap[i]);
			_current_test_failed = 1;
		}
	}
}

static void free_snapshot(char **snap, int nrows) {
	for (int i = 0; i < nrows; i++)
		free(snap[i]);
	free(snap);
}

/* Set mark at (mx, my) and activate it. */
static void set_mark(struct buffer *buf, int mx, int my) {
	buf->markx = mx;
	buf->marky = my;
	buf->mark_active = 1;
}

/* ----------------------------------------------------------------
 * killRectangle tests
 * ---------------------------------------------------------------- */

void test_kill_rect_single_row_undo(void) {
	const char *lines[] = { "ABCDE" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Select columns 1-3 on row 0: kill "BCD" */
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 0);
	killRectangle();

	/* Should now be "AE" */
	TEST_ASSERT_EQUAL_STRING("AE", row_str(buf, 0));

	/* Undo should restore "ABCDE" */
	doUndo(buf, 1);
	assert_buffer_matches(buf, snap, snap_n, "kill_rect_single_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

void test_kill_rect_multi_row_undo(void) {
	const char *lines[] = { "ABCDE", "FGHIJ", "KLMNO" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Rectangle: columns 1-3, rows 0-2 */
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 2);
	killRectangle();

	TEST_ASSERT_EQUAL_STRING("AE", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("FJ", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("KO", row_str(buf, 2));

	doUndo(buf, 1);
	assert_buffer_matches(buf, snap, snap_n, "kill_rect_multi_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

void test_kill_rect_multi_row_redo(void) {
	const char *lines[] = { "ABCDE", "FGHIJ", "KLMNO" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 2);
	killRectangle();

	/* Capture post-kill state */
	int kill_n;
	char **kill_snap = snapshot_buffer(buf, &kill_n);

	doUndo(buf, 1);
	doRedo(buf, 1);

	assert_buffer_matches(buf, kill_snap, kill_n, "kill_rect_redo");

	free_snapshot(kill_snap, kill_n);
	destroyBuffer(buf);
}

void test_kill_rect_short_rows_undo(void) {
	/* Rows shorter than the rectangle right edge */
	const char *lines[] = { "ABCDEFGH", "AB", "ABCDEFGH" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Rectangle: columns 2-6, rows 0-2 */
	buf->cx = 2;
	buf->cy = 0;
	set_mark(buf, 6, 2);
	killRectangle();

	doUndo(buf, 1);
	assert_buffer_matches(buf, snap, snap_n, "kill_rect_short_rows_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

void test_kill_rect_swapped_columns_undo(void) {
	/* Mark column < cursor column — tests normalizeRectCols */
	const char *lines[] = { "ABCDE", "FGHIJ" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 4;
	buf->cy = 0;
	set_mark(buf, 1, 1);
	killRectangle();

	doUndo(buf, 1);
	assert_buffer_matches(buf, snap, snap_n, "kill_rect_swapped_cols_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

/* ----------------------------------------------------------------
 * copyRectangle tests (no undo needed, but verify kill ring)
 * ---------------------------------------------------------------- */

void test_copy_rect_preserves_buffer(void) {
	const char *lines[] = { "ABCDE", "FGHIJ", "KLMNO" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 2);
	copyRectangle();

	/* Buffer must be unchanged */
	assert_buffer_matches(buf, snap, snap_n, "copy_rect_preserves");

	/* Kill ring should have rectangle data */
	TEST_ASSERT_EQUAL_INT(1, E.kill.is_rectangle);
	TEST_ASSERT_EQUAL_INT(3, E.kill.rect_width);
	TEST_ASSERT_EQUAL_INT(3, E.kill.rect_height);

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

/* ----------------------------------------------------------------
 * yankRectangle tests
 * ---------------------------------------------------------------- */

void test_yank_rect_basic_undo(void) {
	const char *lines[] = { "AAAA", "BBBB", "CCCC" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Set up kill ring with a 2-wide, 3-tall rectangle: "XX" per row */
	clearText(&E.kill);
	E.kill.str = (uint8_t *)strdup("XXYYZZ");
	E.kill.is_rectangle = 1;
	E.kill.rect_width = 2;
	E.kill.rect_height = 3;

	buf->cx = 1;
	buf->cy = 0;
	yankRectangle();

	/* Should have inserted XX into each row at column 1 */
	TEST_ASSERT_EQUAL_STRING("AXXAAA", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("BYYBBB", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("CZZCCC", row_str(buf, 2));

	doUndo(buf, 1);
	assert_buffer_matches(buf, snap, snap_n, "yank_rect_basic_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

void test_yank_rect_redo(void) {
	const char *lines[] = { "AAAA", "BBBB", "CCCC" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	clearText(&E.kill);
	E.kill.str = (uint8_t *)strdup("XXYYZZ");
	E.kill.is_rectangle = 1;
	E.kill.rect_width = 2;
	E.kill.rect_height = 3;

	buf->cx = 1;
	buf->cy = 0;
	yankRectangle();

	int yank_n;
	char **yank_snap = snapshot_buffer(buf, &yank_n);

	doUndo(buf, 1);
	doRedo(buf, 1);
	assert_buffer_matches(buf, yank_snap, yank_n, "yank_rect_redo");

	free_snapshot(yank_snap, yank_n);
	destroyBuffer(buf);
}

void test_yank_rect_extra_lines_undo(void) {
	/* Yanking a 4-row rectangle into a 2-row buffer requires
	 * adding extra lines.  Tests the 3-undo-record sequence. */
	const char *lines[] = { "AA", "BB" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	clearText(&E.kill);
	E.kill.str = (uint8_t *)strdup("XXYYZZWW");
	E.kill.is_rectangle = 1;
	E.kill.rect_width = 2;
	E.kill.rect_height = 4;

	buf->cx = 0;
	buf->cy = 0;
	yankRectangle();

	/* Should have 4 rows now */
	TEST_ASSERT_EQUAL_INT(4, buf->numrows);

	doUndo(buf, 1);

	/* Undo should fully restore original buffer */
	assert_buffer_matches(buf, snap, snap_n, "yank_rect_extra_lines_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

void test_yank_rect_into_short_rows_undo(void) {
	/* Rows shorter than the insertion column — tests space padding.
	 * Undo must remove both the inserted rectangle data AND the
	 * space padding added to reach the insertion column. */
	const char *lines[] = { "A", "B", "C" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	clearText(&E.kill);
	E.kill.str = (uint8_t *)strdup("XXYYZZ");
	E.kill.is_rectangle = 1;
	E.kill.rect_width = 2;
	E.kill.rect_height = 3;

	/* Insert at column 5 — well past end of each 1-char row */
	buf->cx = 5;
	buf->cy = 0;
	yankRectangle();

	doUndo(buf, 1);

	/* Undo should fully restore original buffer */
	assert_buffer_matches(buf, snap, snap_n, "yank_rect_short_rows_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

/* ----------------------------------------------------------------
 * deleteRange tests (linear region, for comparison)
 * ---------------------------------------------------------------- */

void test_delete_range_single_line_undo(void) {
	struct buffer *buf = make_test_buffer("Hello World");

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	deleteRange(5, 0, 11, 0, 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));

	doUndo(buf, 1);
	assert_buffer_matches(buf, snap, snap_n, "delete_range_single_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

void test_delete_range_multi_line_undo(void) {
	const char *lines[] = { "Hello", "Beautiful", "World" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Delete from (3,0) to (2,2): "lo\nBeautiful\nWo" */
	deleteRange(3, 0, 2, 2, 1);

	doUndo(buf, 1);
	assert_buffer_matches(buf, snap, snap_n, "delete_range_multi_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

/* ----------------------------------------------------------------
 * killRegion (linear) undo — baseline for comparison
 * ---------------------------------------------------------------- */

void test_kill_region_undo(void) {
	const char *lines[] = { "Hello", "World" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 2;
	buf->cy = 0;
	set_mark(buf, 3, 1);
	killRegion();

	doUndo(buf, 1);
	assert_buffer_matches(buf, snap, snap_n, "kill_region_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

/* ----------------------------------------------------------------
 * Round-trip: kill rectangle then yank it back
 * ---------------------------------------------------------------- */

void test_kill_then_yank_rect_round_trip(void) {
	const char *lines[] = { "ABCDE", "FGHIJ", "KLMNO" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* Kill columns 1-4 */
	buf->cx = 1;
	buf->cy = 0;
	set_mark(buf, 4, 2);
	killRectangle();

	/* Now yank it back at the same position */
	buf->cx = 1;
	buf->cy = 0;
	yankRectangle();

	/* Should be back to original content */
	assert_buffer_matches(buf, snap, snap_n, "kill_yank_round_trip");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

/* ----------------------------------------------------------------
 * Stress: undo through multiple rectangle operations
 * ---------------------------------------------------------------- */

void test_multiple_rect_ops_undo_all(void) {
	const char *lines[] = { "AAAA", "BBBB", "CCCC" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	/* First: kill rectangle cols 0-2 */
	buf->cx = 0;
	buf->cy = 0;
	set_mark(buf, 2, 2);
	buf->mark_active = 1;
	killRectangle();

	/* Second: yank some rectangle */
	clearText(&E.kill);
	E.kill.str = (uint8_t *)strdup("XXYYZZ");
	E.kill.is_rectangle = 1;
	E.kill.rect_width = 2;
	E.kill.rect_height = 3;
	buf->cx = 0;
	buf->cy = 0;
	yankRectangle();

	/* Undo yank */
	doUndo(buf, 1);
	/* Undo kill */
	doUndo(buf, 1);

	assert_buffer_matches(buf, snap, snap_n, "multiple_rect_ops_undo");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

/* ----------------------------------------------------------------
 * Edge case: zero-width rectangle (topx == botx)
 * ---------------------------------------------------------------- */

void test_kill_rect_zero_width(void) {
	const char *lines[] = { "ABCDE" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);

	int snap_n;
	char **snap = snapshot_buffer(buf, &snap_n);

	buf->cx = 2;
	buf->cy = 0;
	set_mark(buf, 2, 0);
	/* markInvalid will catch this (same point), so buffer stays intact */
	killRectangle();

	assert_buffer_matches(buf, snap, snap_n, "kill_rect_zero_width");

	free_snapshot(snap, snap_n);
	destroyBuffer(buf);
}

/* ----------------------------------------------------------------
 * Runner
 * ---------------------------------------------------------------- */

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
}

int main(void) {
	TEST_BEGIN();

	/* Kill rectangle */
	RUN_TEST(test_kill_rect_single_row_undo);
	RUN_TEST(test_kill_rect_multi_row_undo);
	RUN_TEST(test_kill_rect_multi_row_redo);
	RUN_TEST(test_kill_rect_short_rows_undo);
	RUN_TEST(test_kill_rect_swapped_columns_undo);

	/* Copy rectangle */
	RUN_TEST(test_copy_rect_preserves_buffer);

	/* Yank rectangle */
	RUN_TEST(test_yank_rect_basic_undo);
	RUN_TEST(test_yank_rect_redo);
	RUN_TEST(test_yank_rect_extra_lines_undo);
	RUN_TEST(test_yank_rect_into_short_rows_undo);

	/* Linear delete/kill (baseline) */
	RUN_TEST(test_delete_range_single_line_undo);
	RUN_TEST(test_delete_range_multi_line_undo);
	RUN_TEST(test_kill_region_undo);

	/* Round-trip */
	RUN_TEST(test_kill_then_yank_rect_round_trip);

	/* Multiple operations */
	RUN_TEST(test_multiple_rect_ops_undo_all);

	/* Edge cases */
	RUN_TEST(test_kill_rect_zero_width);

	return TEST_END();
}
