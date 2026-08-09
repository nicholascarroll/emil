/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_abuf.c: Tests for the append buffer (abuf).
 *
 * abuf is a simple dynamic byte buffer used for screen rendering.
 * These tests exercise the realloc doubling and edge cases. */

#include "test.h"
#include "test_harness.h"
#include "abuf.h"
#include <string.h>
#include <stdlib.h>

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

void test_abuf_append_to_empty(void) {
	struct abuf ab = ABUF_INIT;
	abAppend(&ab, "hello", 5);
	TEST_ASSERT_EQUAL_INT(5, ab.len);
	TEST_ASSERT(ab.capacity >= 5);
	TEST_ASSERT(memcmp(ab.b, "hello", 5) == 0);
	abFree(&ab);
}

void test_abuf_zero_length_append(void) {
	struct abuf ab = ABUF_INIT;
	abAppend(&ab, "hello", 5);
	abAppend(&ab, "", 0);
	TEST_ASSERT_EQUAL_INT(5, ab.len);
	TEST_ASSERT(memcmp(ab.b, "hello", 5) == 0);
	abFree(&ab);
}

void test_abuf_force_multiple_doublings(void) {
	struct abuf ab = ABUF_INIT;
	/* Initial capacity is 8192.  Append enough to force at least
	 * one doubling. */
	char block[1024];
	memset(block, 'x', sizeof(block));
	for (int i = 0; i < 20; i++)
		abAppend(&ab, block, sizeof(block));
	TEST_ASSERT_EQUAL_INT(20 * 1024, ab.len);
	TEST_ASSERT(ab.capacity >= 20 * 1024);
	/* Verify content integrity */
	for (int i = 0; i < ab.len; i++)
		TEST_ASSERT(ab.b[i] == 'x');
	abFree(&ab);
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_abuf_append_to_empty);
	RUN_TEST(test_abuf_zero_length_append);
	RUN_TEST(test_abuf_force_multiple_doublings);

	return TEST_END();
}
