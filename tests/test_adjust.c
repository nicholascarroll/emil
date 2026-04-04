/* test_adjust.c — Tests for adjustPoint and adjustAllPoints.
 *
 * adjustPoint is pure logic: given a tracked point and a mutation range,
 * compute the new point position.  These tests cover every branch for
 * both insertions and deletions, plus edge cases at exact boundaries. */

#include "test.h"
#include "test_harness.h"
#include "adjust.h"
#include <string.h>

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- Deletion: point before region ---- */

void test_del_point_before_same_line(void) {
	int px = 2, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_del_point_before_earlier_line(void) {
	int px = 5, py = 1;
	int ret = adjustPoint(&px, &py, 3, 5, 8, 7, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(1, py);
}

void test_del_point_at_start(void) {
	/* Point at exact start of deletion — "before" branch (<=) */
	int px = 5, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

/* ---- Deletion: point inside region ---- */

void test_del_point_inside_single_line(void) {
	int px = 7, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(1, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_del_point_inside_multiline(void) {
	/* Delete from (5,3) to (8,7), point at (2,5) is inside */
	int px = 2, py = 5;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 7, 1);
	TEST_ASSERT_EQUAL_INT(1, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_del_point_at_end(void) {
	/* Point at exact end of deletion — inside (<=) */
	int px = 10, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(1, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

/* ---- Deletion: point after region, same end line ---- */

void test_del_point_after_on_end_line(void) {
	/* Delete (5,3) to (10,3), point at (15,3) → should shift left */
	int px = 15, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(10, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_del_point_after_on_end_line_multiline(void) {
	/* Delete (5,3) to (8,7), point at (20,7) → col adjusts to start line */
	int px = 20, py = 7;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 7, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(17, px); /* 5 + (20 - 8) */
	TEST_ASSERT_EQUAL_INT(3, py);
}

/* ---- Deletion: point after region, later line ---- */

void test_del_point_after_later_line(void) {
	/* Delete (5,3) to (8,7), point at (2,10) → row shifts up by 4 */
	int px = 2, py = 10;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 7, 1);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(6, py); /* 10 - (7-3) */
}

/* ---- Insertion: point before region ---- */

void test_ins_point_before_same_line(void) {
	int px = 2, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_ins_point_before_earlier_line(void) {
	int px = 5, py = 1;
	int ret = adjustPoint(&px, &py, 3, 5, 8, 7, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(5, px);
	TEST_ASSERT_EQUAL_INT(1, py);
}

/* ---- Insertion: point at insertion point (same-line) ---- */

void test_ins_point_at_start_same_line(void) {
	/* Insert 5 chars at (5,3), ending at (10,3), point at (5,3) */
	int px = 5, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(10, px); /* shifted right by 5 */
	TEST_ASSERT_EQUAL_INT(3, py);
}

void test_ins_point_after_on_same_line(void) {
	/* Insert at (5,3) ending at (10,3), point at (8,3) → shifts right */
	int px = 8, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(13, px); /* 8 + (10-5) */
	TEST_ASSERT_EQUAL_INT(3, py);
}

/* ---- Insertion: point on insertion line, multi-line insert ---- */

void test_ins_multiline_point_on_start_line(void) {
	/* Insert at (5,3) ending at (8,5), point at (10,3) */
	int px = 10, py = 3;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 5, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(13, px); /* 8 + (10 - 5) */
	TEST_ASSERT_EQUAL_INT(5, py);  /* 3 + (5-3) */
}

/* ---- Insertion: point after insertion line ---- */

void test_ins_point_after_later_line(void) {
	/* Insert at (5,3) ending at (8,5), point at (2,10) → row shifts down */
	int px = 2, py = 10;
	int ret = adjustPoint(&px, &py, 5, 3, 8, 5, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(12, py); /* 10 + (5-3) */
}

void test_ins_point_after_single_line(void) {
	/* Single-line insert, point on later line — no column change */
	int px = 2, py = 10;
	int ret = adjustPoint(&px, &py, 5, 3, 10, 3, 0);
	TEST_ASSERT_EQUAL_INT(0, ret);
	TEST_ASSERT_EQUAL_INT(2, px);
	TEST_ASSERT_EQUAL_INT(10, py); /* no line delta */
}

int main(void) {
	TEST_BEGIN();

	/* Deletion */
	RUN_TEST(test_del_point_before_same_line);
	RUN_TEST(test_del_point_before_earlier_line);
	RUN_TEST(test_del_point_at_start);
	RUN_TEST(test_del_point_inside_single_line);
	RUN_TEST(test_del_point_inside_multiline);
	RUN_TEST(test_del_point_at_end);
	RUN_TEST(test_del_point_after_on_end_line);
	RUN_TEST(test_del_point_after_on_end_line_multiline);
	RUN_TEST(test_del_point_after_later_line);

	/* Insertion */
	RUN_TEST(test_ins_point_before_same_line);
	RUN_TEST(test_ins_point_before_earlier_line);
	RUN_TEST(test_ins_point_at_start_same_line);
	RUN_TEST(test_ins_point_after_on_same_line);
	RUN_TEST(test_ins_multiline_point_on_start_line);
	RUN_TEST(test_ins_point_after_later_line);
	RUN_TEST(test_ins_point_after_single_line);

	return TEST_END();
}
