/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_insert_file.c: characterisation for M-x insert-file.
 */

#include "test.h"
#include "test_harness.h"
#include "emil.h"
#include "buffer.h"
#include "fileio.h"
#include "undo.h"
#include "edit.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>



void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* Write `content` to a fresh temp file; return mallocd path. */
static char *make_temp_file(const char *content) {
	static char tmpname[64];
	strcpy(tmpname, "/tmp/emil_insfile_XXXXXX");
	int fd = mkstemp(tmpname);
	if (fd < 0)
		return NULL;
	if (content && *content)
		(void)!write(fd, content, strlen(content));
	close(fd);
	return strdup(tmpname);
}

/* --- 1. insertFile dirties and is undoable ------------------------ */

void test_insert_file_is_dirty_and_undoable(void) {
	char *path = make_temp_file("aaa\nbbb\n");
	TEST_ASSERT_NOT_NULL(path);

	/* Start with a non-empty buffer so we can exercise the
	 * push-down-existing-row case. */
	struct buffer *buf = make_test_buffer("existing");
	TEST_ASSERT_FALSE(buf->dirty);
	TEST_ASSERT_NULL(buf->undo);

	buf->cx = 0;
	buf->cy = 0;
	int rc = insertFileAtPath(buf, path, path);
	TEST_ASSERT_EQUAL_INT(0, rc);

	/* Buffer is dirty, rows match, cursor at end of "bbb". */
	TEST_ASSERT_TRUE(buf->dirty);
	TEST_ASSERT_EQUAL_INT(4, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("aaa", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("bbb", row_str(buf, 1));
	TEST_ASSERT_EQUAL_STRING("existing", row_str(buf, 2));
	TEST_ASSERT_EQUAL_INT(1, buf->cy);
	TEST_ASSERT_EQUAL_INT(3, buf->cx);

	/* Undo stack is populated — insert-file left something to undo. */
	TEST_ASSERT_NOT_NULL(buf->undo);

	/* C-_ reverts the insertion.  After undo: original single row,
	 * no extra content. */
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("existing", row_str(buf, 0));

	unlink(path);
	free(path);
}

/* --- 2. insertFile into empty buffer does not manufacture rows ---- */

void test_insert_file_empty_buffer_no_trailing_row(void) {
	char *path = make_temp_file("aaa\nbbb\n");
	TEST_ASSERT_NOT_NULL(path);

	/* Fresh buffer: one empty row, which is what the empty buffer
	 * is under the #105 representation. */
	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);

	buf->cx = 0;
	buf->cy = 0;
	int rc = insertFileAtPath(buf, path, path);
	TEST_ASSERT_EQUAL_INT(0, rc);

	/* The file's two lines, plus the final empty row that carries
	 * its trailing newline -- "aaa\nbbb\n" exactly.  No spurious
	 * row beyond that. */
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("aaa", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("bbb", row_str(buf, 1));

	unlink(path);
	free(path);
}

/* --- 3. insertFile rejects nonexistent files ---------------------- */

void test_insert_file_nonexistent_returns_error(void) {
	struct buffer *buf = make_test_buffer("existing");
	TEST_ASSERT_FALSE(buf->dirty);

	int rc = insertFileAtPath(buf, "/tmp/this_file_does_not_exist_xyzzy",
				  NULL);
	TEST_ASSERT(rc != 0);

	/* Buffer untouched. */
	TEST_ASSERT_FALSE(buf->dirty);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("existing", row_str(buf, 0));
}

/* --- 4. insertFile refuses read-only buffers ---------------------- */

void test_insert_file_readonly(void) {
	char *path = make_temp_file("aaa\nbbb\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *buf = make_test_buffer("existing");
	buf->read_only = 1;

	int rc = insertFileAtPath(buf, path, path);
	TEST_ASSERT_EQUAL_INT(1, rc);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("existing", row_str(buf, 0));

	buf->read_only = 0;
	unlink(path);
	free(path);
}

/* --- runner ------------------------------------------------------- */

/* --- 5. insert-file undo round-trips wherever point is ----------- */

/* insert-file is a bulk mutation: its record stands alone and never
 * joins a typing run.  The virtual-EOF case is the one #102 got wrong,
 * so it is pinned from both sides of the anchoring rule. */
void test_insert_file_undo_round_trips(void) {
	const char *lines[] = { "one", "two" };

	struct buffer *mid = make_test_buffer_lines(lines, 2);
	char *path = make_temp_file("aaa\nbbb\n");
	TEST_ASSERT_NOT_NULL(path);
	mid->cy = 0;
	mid->cx = 0;
	insertFileAtPath(mid, path, NULL);
	TEST_ASSERT_EQUAL_INT(5, mid->numrows);
	while (mid->undo)
		doUndo(mid, 1);
	TEST_ASSERT_EQUAL_INT(3, mid->numrows);
	TEST_ASSERT_EQUAL_STRING("one", row_str(mid, 0));
	TEST_ASSERT_EQUAL_STRING("two", row_str(mid, 1));

	struct buffer *eof = make_test_buffer_lines(lines, 2);
	eof->next = mid; /* keep both reachable for cleanup */
	eof->cy = 2;	 /* virtual EOF line */
	eof->cx = 0;
	insertFileAtPath(eof, path, NULL);
	TEST_ASSERT_EQUAL_INT(5, eof->numrows);
	while (eof->undo)
		doUndo(eof, 1);
	TEST_ASSERT_EQUAL_INT(3, eof->numrows);
	TEST_ASSERT_EQUAL_STRING("one", row_str(eof, 0));
	TEST_ASSERT_EQUAL_STRING("two", row_str(eof, 1));

	free(path);
}

/* Typing either side of a file insertion must not fold across it. */
void test_insert_file_does_not_join_a_typing_run(void) {
	const char *lines[] = { "one", "two" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	char *path = make_temp_file("zzz\n");
	TEST_ASSERT_NOT_NULL(path);

	buf->cy = 0;
	buf->cx = 3;
	E.buf = buf;
	clearUndosAndRedos(buf);

	selfInsert(buf, 'X', 1);
	buf->cy = 0;
	buf->cx = 0;
	insertFileAtPath(buf, path, NULL);
	buf->cy = 1;
	buf->cx = buf->row[1].size;
	selfInsert(buf, 'Y', 1);

	while (buf->undo)
		doUndo(buf, 1);

	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("one", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("two", row_str(buf, 1));

	free(path);
}

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_insert_file_is_dirty_and_undoable);
	RUN_TEST(test_insert_file_empty_buffer_no_trailing_row);
	RUN_TEST(test_insert_file_nonexistent_returns_error);
	RUN_TEST(test_insert_file_readonly);
	RUN_TEST(test_insert_file_undo_round_trips);
	RUN_TEST(test_insert_file_does_not_join_a_typing_run);
	return TEST_END();
}
