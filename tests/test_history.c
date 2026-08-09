/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_history.c: Tests for addHistory, getHistoryAt, freeHistory.
 *
 * History is a doubly-linked list with duplicate suppression and
 * eviction at HISTORY_MAX_ENTRIES. */

#include "test.h"
#include "prompt.h"
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
	/* Fill past the limit.  Sized for "entry_" plus the widest int
	 * the compiler can see the loop counter taking, so no truncation
	 * is possible. */
	char buf[32];
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


/* B8 — Down in a prompt destroys typed text
 *
 * With history_pos == -1 (the user has not browsed history at all),
 * Down falls into the else branch, leaves history_pos at -1, and hits
 * replaceMinibufferText(E.minibuf, "").  The typed text is gone.
 *
 * Note the history must be non-empty for the bug to fire: the whole
 * block is guarded by `hist->count > 0`. */

void test_b8_down_without_browsing_keeps_typed_text(void) {
	makeMinibuffer();
	struct buffer *file = make_test_buffer("contents");
	addHistory(&E.file_history, "/tmp/previously-visited");

	int keys[] = { '/', 't', 'm', 'p', '/', 'a', 'b', 'c',
		       KEY_ARROW_DOWN, '\r' };
	scriptKeys(keys, 10);

	muteStdout();
	uint8_t *r = editorPrompt(file, "Find File: ", PROMPT_FILES, NULL);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_NOT_NULL(r);
	if (r)
		TEST_ASSERT_EQUAL_STRING("/tmp/abc", (const char *)r);

	free(r);
	freeMinibuffer();
}

/* The Emacs behaviour of the same key pair: Up parks the in-progress
 * text, Down brings it back.  This is the "full fix" option (decision
 * 2 in the audit); the minimal fix only makes the test above pass. */
void test_b8_up_then_down_restores_typed_text(void) {
	makeMinibuffer();
	struct buffer *file = make_test_buffer("contents");
	addHistory(&E.file_history, "/tmp/previously-visited");

	int keys[] = { '/', 't', 'm', 'p', '/', 'a', 'b', 'c',
		       KEY_ARROW_UP, KEY_ARROW_DOWN, '\r' };
	scriptKeys(keys, 11);

	muteStdout();
	uint8_t *r = editorPrompt(file, "Find File: ", PROMPT_FILES, NULL);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_NOT_NULL(r);
	if (r)
		TEST_ASSERT_EQUAL_STRING("/tmp/abc", (const char *)r);

	free(r);
	freeMinibuffer();
}

int main(void) {
	TEST_BEGIN();

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


	RUN_TEST(test_b8_down_without_browsing_keeps_typed_text);
	RUN_TEST(test_b8_up_then_down_restores_typed_text);
	return TEST_END();
}
