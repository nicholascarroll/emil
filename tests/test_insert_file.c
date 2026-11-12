/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_insert_file.c: characterisation for insert-file (C-x i).
 *
 * Each test issues the command, types the file's path and RET at the
 * prompt it opens, and reads the buffer text, cursor and modified
 * flag.  Nothing here names the function that does the loading, so
 * the loaders can be merged without touching this file.
 */

#include "test.h"
#include "test_harness.h"
#include "emil.h"
#include "buffer.h"
#include "fileio.h"
#include "keymap.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>



void setUp(void) {
	initTestEditor();
	makeMinibuffer();
}
void tearDown(void) {
	freeMinibuffer();
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

/* Write `content` to a fresh temp file; return mallocd path. */
static char *make_temp_file(const char *content) {
	static char tmpname[PATH_MAX];
	testTmpPath(tmpname, sizeof(tmpname), "emil_insfile_XXXXXX");
	int fd = mkstemp(tmpname);
	if (fd < 0)
		return NULL;
	if (content && *content)
		(void)!write(fd, content, strlen(content));
	close(fd);
	return strdup(tmpname);
}

/* C-x i, with `path` and RET waiting at the prompt.  Returns how many
 * of those keys were read: all of them when the command prompted,
 * none when it refused first. */
static int insert_file_typing(const char *path) {
	int keys[PATH_MAX + 1];
	int n = 0;
	for (const char *p = path; *p; p++)
		keys[n++] = (unsigned char)*p;
	keys[n++] = '\r';

	scriptKeys(keys, n);
	muteStdout();
	processKeypress(CMD_INSERT_FILE);
	unmuteStdout();
	int used = test_key_pos;
	clearKeys();
	return used;
}

/* Type one character at point. */
static void type_char(int ch) {
	E.self_insert_key = ch;
	processKeypress(CMD_SELF_INSERT);
}

/* --- 1. insert-file dirties and is undoable ----------------------- */

void test_insert_file_is_dirty_and_undoable(void) {
	char *path = make_temp_file("aaa\nbbb\n");
	TEST_ASSERT_NOT_NULL(path);

	/* Start with a non-empty buffer so we can exercise the
	 * push-down-existing-row case. */
	struct buffer *buf = make_test_buffer("existing");
	TEST_ASSERT_FALSE(buf->dirty);

	buf->cx = 0;
	buf->cy = 0;
	int used = insert_file_typing(path);
	TEST_ASSERT_EQUAL_INT((int)strlen(path) + 1, used);

	/* Buffer is dirty, the file's lines sit above the old one,
	 * cursor at end of "bbb". */
	TEST_ASSERT_TRUE(buf->dirty);
	ASSERT_TEXT("aaa\nbbb\nexisting\n", buf);
	TEST_ASSERT_EQUAL_INT(1, buf->cy);
	TEST_ASSERT_EQUAL_INT(3, buf->cx);

	/* C-_ reverts the insertion.  After undo: the original text,
	 * no extra content. */
	processKeypress(CMD_UNDO);
	ASSERT_TEXT("existing\n", buf);

	unlink(path);
	free(path);
}

/* --- 2. insert-file into empty buffer does not manufacture rows --- */

void test_insert_file_empty_buffer_no_trailing_row(void) {
	char *path = make_temp_file("aaa\nbbb\n");
	TEST_ASSERT_NOT_NULL(path);

	/* Fresh buffer: nothing in it. */
	struct buffer *buf = make_test_buffer(NULL);
	ASSERT_TEXT("", buf);

	buf->cx = 0;
	buf->cy = 0;
	insert_file_typing(path);

	/* The buffer now holds exactly the file's bytes: its two
	 * lines and its trailing newline, nothing beyond that. */
	ASSERT_TEXT("aaa\nbbb\n", buf);

	unlink(path);
	free(path);
}

/* --- 3. insert-file rejects nonexistent files --------------------- */

void test_insert_file_nonexistent_returns_error(void) {
	struct buffer *buf = make_test_buffer("existing");
	TEST_ASSERT_FALSE(buf->dirty);

	char absent[PATH_MAX];
	testTmpPath(absent, sizeof(absent), "this_file_does_not_exist_xyzzy");
	int used = insert_file_typing(absent);

	/* The prompt was answered... */
	TEST_ASSERT_EQUAL_INT((int)strlen(absent) + 1, used);

	/* ...and the buffer is untouched. */
	TEST_ASSERT_FALSE(buf->dirty);
	ASSERT_TEXT("existing\n", buf);
}

/* --- 4. insert-file refuses read-only buffers --------------------- */

void test_insert_file_readonly(void) {
	char *path = make_temp_file("aaa\nbbb\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *buf = make_test_buffer("existing");
	buf->read_only = 1;

	/* Refused before the prompt: the path is never asked for. */
	int used = insert_file_typing(path);
	TEST_ASSERT_EQUAL_INT(0, used);
	ASSERT_TEXT("existing\n", buf);
	TEST_ASSERT_FALSE(buf->dirty);

	buf->read_only = 0;
	unlink(path);
	free(path);
}

/* --- 5. insert-file undo round-trips wherever point is ----------- */

/* insert-file is a bulk mutation: its record stands alone and never
 * joins a typing run.  The virtual-EOF case is the one #102 got wrong,
 * so it is pinned from both sides of the anchoring rule. */
void test_insert_file_undo_round_trips(void) {
	const char *lines[] = { "one", "two" };
	char *path = make_temp_file("aaa\nbbb\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *mid = make_test_buffer_lines(lines, 2);
	mid->cy = 0;
	mid->cx = 0;
	insert_file_typing(path);
	ASSERT_TEXT("aaa\nbbb\none\ntwo\n", mid);
	processKeypress(CMD_UNDO);
	ASSERT_TEXT("one\ntwo\n", mid);

	struct buffer *eof = make_test_buffer_lines(lines, 2);
	eof->next = mid; /* keep both reachable for cleanup */
	eof->cy = 2;	 /* virtual EOF line */
	eof->cx = 0;
	insert_file_typing(path);
	ASSERT_TEXT("one\ntwo\naaa\nbbb\n", eof);
	processKeypress(CMD_UNDO);
	ASSERT_TEXT("one\ntwo\n", eof);

	unlink(path);
	free(path);
}

/* Typing either side of a file insertion must not fold across it:
 * each of the three comes back out with its own undo. */
void test_insert_file_does_not_join_a_typing_run(void) {
	const char *lines[] = { "one", "two" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	char *path = make_temp_file("zzz\n");
	TEST_ASSERT_NOT_NULL(path);

	buf->cy = 0;
	buf->cx = 3;
	type_char('X');
	ASSERT_TEXT("oneX\ntwo\n", buf);

	buf->cy = 0;
	buf->cx = 0;
	insert_file_typing(path);
	ASSERT_TEXT("zzz\noneX\ntwo\n", buf);

	buf->cy = 1;
	buf->cx = buf->row[1].size;
	type_char('Y');
	ASSERT_TEXT("zzz\noneXY\ntwo\n", buf);

	processKeypress(CMD_UNDO); /* the Y */
	ASSERT_TEXT("zzz\noneX\ntwo\n", buf);
	processKeypress(CMD_UNDO); /* the file */
	ASSERT_TEXT("oneX\ntwo\n", buf);
	processKeypress(CMD_UNDO); /* the X */
	ASSERT_TEXT("one\ntwo\n", buf);

	unlink(path);
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
