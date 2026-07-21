/* test_insert_file.c: characterisation for M-x insert-file.
 */

#include "test.h"
#include "test_harness.h"
#include "emil.h"
#include "buffer.h"
#include "fileio.h"
#include "undo.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern struct config E;

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
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
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
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("existing", row_str(buf, 0));

	unlink(path);
	free(path);
}

/* --- 2. insertFile into empty buffer does not manufacture rows ---- */

void test_insert_file_empty_buffer_no_trailing_row(void) {
	char *path = make_temp_file("aaa\nbbb\n");
	TEST_ASSERT_NOT_NULL(path);

	/* Fresh buffer with numrows == 0 — the past-end virtual-row case. */
	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, buf->numrows);

	buf->cx = 0;
	buf->cy = 0;
	int rc = insertFileAtPath(buf, path, path);
	TEST_ASSERT_EQUAL_INT(0, rc);

	/* Two rows, no spurious trailing empty row. */
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
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
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
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
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("existing", row_str(buf, 0));

	buf->read_only = 0;
	unlink(path);
	free(path);
}

/* --- runner ------------------------------------------------------- */

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_insert_file_is_dirty_and_undoable);
	RUN_TEST(test_insert_file_empty_buffer_no_trailing_row);
	RUN_TEST(test_insert_file_nonexistent_returns_error);
	RUN_TEST(test_insert_file_readonly);
	return TEST_END();
}
