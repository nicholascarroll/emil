/* test_history.c — Tests for addHistory, getHistoryAt, freeHistory.
 *
 * History is a doubly-linked list with duplicate suppression and
 * eviction at HISTORY_MAX_ENTRIES. */

#include "test.h"
#include "test_harness.h"
#include "history.h"
#include <string.h>
#include <stdlib.h>

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- Basic add and retrieve ---- */

void test_history_add_one(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, "hello");
	TEST_ASSERT_EQUAL_INT(1, h.count);
	struct historyEntry *e = getHistoryAt(&h, 0);
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_EQUAL_STRING("hello", e->str);
	freeHistory(&h);
}

void test_history_add_multiple(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, "first");
	addHistory(&h, "second");
	addHistory(&h, "third");
	TEST_ASSERT_EQUAL_INT(3, h.count);
	/* Index 0 is oldest (head), index 2 is newest (tail) */
	TEST_ASSERT_EQUAL_STRING("first", getHistoryAt(&h, 0)->str);
	TEST_ASSERT_EQUAL_STRING("second", getHistoryAt(&h, 1)->str);
	TEST_ASSERT_EQUAL_STRING("third", getHistoryAt(&h, 2)->str);
	freeHistory(&h);
}

void test_history_get_last(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, "alpha");
	addHistory(&h, "beta");
	struct historyEntry *last = getLastHistory(&h);
	TEST_ASSERT_NOT_NULL(last);
	TEST_ASSERT_EQUAL_STRING("beta", last->str);
	freeHistory(&h);
}

/* ---- Duplicate suppression ---- */

void test_history_no_consecutive_duplicate(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, "same");
	addHistory(&h, "same");
	TEST_ASSERT_EQUAL_INT(1, h.count);
	freeHistory(&h);
}

void test_history_non_consecutive_duplicate_allowed(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, "aaa");
	addHistory(&h, "bbb");
	addHistory(&h, "aaa");
	TEST_ASSERT_EQUAL_INT(3, h.count);
	freeHistory(&h);
}

/* ---- Empty and NULL ---- */

void test_history_add_null(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, NULL);
	TEST_ASSERT_EQUAL_INT(0, h.count);
	freeHistory(&h);
}

void test_history_add_empty_string(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, "");
	TEST_ASSERT_EQUAL_INT(0, h.count);
	freeHistory(&h);
}

/* ---- Out-of-range index ---- */

void test_history_get_negative_index(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, "x");
	TEST_ASSERT_NULL(getHistoryAt(&h, -1));
	freeHistory(&h);
}

void test_history_get_past_end(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, "x");
	TEST_ASSERT_NULL(getHistoryAt(&h, 1));
	TEST_ASSERT_NULL(getHistoryAt(&h, 100));
	freeHistory(&h);
}

void test_history_get_empty(void) {
	struct history h;
	initHistory(&h);
	TEST_ASSERT_NULL(getHistoryAt(&h, 0));
	freeHistory(&h);
}

/* ---- Eviction ---- */

void test_history_eviction(void) {
	struct history h;
	initHistory(&h);
	/* Fill past the limit */
	char buf[16];
	for (int i = 0; i < HISTORY_MAX_ENTRIES + 5; i++) {
		snprintf(buf, sizeof(buf), "entry_%d", i);
		addHistory(&h, buf);
	}
	TEST_ASSERT_EQUAL_INT(HISTORY_MAX_ENTRIES, h.count);
	/* Oldest entries should have been evicted; head is entry_5 */
	TEST_ASSERT_EQUAL_STRING("entry_5", getHistoryAt(&h, 0)->str);
	/* Newest is the last one added */
	snprintf(buf, sizeof(buf), "entry_%d", HISTORY_MAX_ENTRIES + 4);
	struct historyEntry *last = getLastHistory(&h);
	TEST_ASSERT_NOT_NULL(last);
	TEST_ASSERT_EQUAL_STRING(buf, last->str);
	freeHistory(&h);
}

/* ---- Rectangle metadata ---- */

void test_history_rect_metadata(void) {
	struct history h;
	initHistory(&h);
	addHistoryWithRect(&h, "rect_data", 1, 10, 3);
	TEST_ASSERT_EQUAL_INT(1, h.count);
	struct historyEntry *e = getHistoryAt(&h, 0);
	TEST_ASSERT_NOT_NULL(e);
	TEST_ASSERT_EQUAL_INT(1, e->is_rectangle);
	TEST_ASSERT_EQUAL_INT(10, e->rect_width);
	TEST_ASSERT_EQUAL_INT(3, e->rect_height);
	freeHistory(&h);
}

void test_history_rect_duplicate_suppression(void) {
	struct history h;
	initHistory(&h);
	addHistoryWithRect(&h, "data", 1, 10, 3);
	addHistoryWithRect(&h, "data", 1, 10, 3);
	TEST_ASSERT_EQUAL_INT(1, h.count);
	/* Same string but different rect metadata — not a duplicate */
	addHistoryWithRect(&h, "data", 1, 20, 3);
	TEST_ASSERT_EQUAL_INT(2, h.count);
	freeHistory(&h);
}

/* ---- freeHistory resets ---- */

void test_history_free_resets(void) {
	struct history h;
	initHistory(&h);
	addHistory(&h, "x");
	addHistory(&h, "y");
	freeHistory(&h);
	TEST_ASSERT_EQUAL_INT(0, h.count);
	TEST_ASSERT_NULL(h.head);
	TEST_ASSERT_NULL(h.tail);
	/* Should be safe to reuse */
	addHistory(&h, "z");
	TEST_ASSERT_EQUAL_INT(1, h.count);
	freeHistory(&h);
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_history_add_one);
	RUN_TEST(test_history_add_multiple);
	RUN_TEST(test_history_get_last);
	RUN_TEST(test_history_no_consecutive_duplicate);
	RUN_TEST(test_history_non_consecutive_duplicate_allowed);
	RUN_TEST(test_history_add_null);
	RUN_TEST(test_history_add_empty_string);
	RUN_TEST(test_history_get_negative_index);
	RUN_TEST(test_history_get_past_end);
	RUN_TEST(test_history_get_empty);
	RUN_TEST(test_history_eviction);
	RUN_TEST(test_history_rect_metadata);
	RUN_TEST(test_history_rect_duplicate_suppression);
	RUN_TEST(test_history_free_resets);

	return TEST_END();
}
