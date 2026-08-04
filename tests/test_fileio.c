/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_fileio.c: File I/O, round-trip, UTF-8 validation, emil_getline. */

#include "test.h"
#include "test_harness.h"
#include "fileio.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- emil_getline ---- */

void test_getline_short(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	fputs("Hello, World!\n", fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(14, r);
	TEST_ASSERT_EQUAL_STRING("Hello, World!\n", line);

	free(line);
	fclose(fp);
}

void test_getline_exact_120(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	for (int i = 0; i < 119; i++)
		fputc('A', fp);
	fputc('\n', fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(120, r);
	TEST_ASSERT(line[119] == '\n');
	TEST_ASSERT(line[120] == '\0');

	free(line);
	fclose(fp);
}

void test_getline_long(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	for (int i = 0; i < 200; i++)
		fputc('0' + (i % 10), fp);
	fputc('\n', fp);
	fputs("Second line\n", fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;

	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(201, r);
	TEST_ASSERT(line[0] == '0');
	TEST_ASSERT(line[199] == '9');

	r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(12, r);
	TEST_ASSERT_EQUAL_STRING("Second line\n", line);

	free(line);
	fclose(fp);
}

void test_getline_no_newline(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	fputs("No newline at end", fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(17, r);
	TEST_ASSERT_EQUAL_STRING("No newline at end", line);

	free(line);
	fclose(fp);
}

void test_getline_empty(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(-1, r);

	free(line);
	fclose(fp);
}

void test_getline_multiple_reallocs(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	for (int i = 0; i < 1000; i++)
		fputc('X', fp);
	fputc('\n', fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(1001, r);
	TEST_ASSERT(line[0] == 'X');
	TEST_ASSERT(line[999] == 'X');
	TEST_ASSERT(n >= 1001);

	free(line);
	fclose(fp);
}

/* ---- File round-trip ---- */

void test_rows_to_string(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, (const uint8_t *)"Hello", 5);
	insertRow(buf, 1, (const uint8_t *)"World", 5);
	insertRow(buf, 2, (const uint8_t *)"", 0);

	size_t buflen = 0;
	char *str = rowsToString(buf, &buflen);
	TEST_ASSERT_EQUAL_INT(13, (int)buflen);
	TEST_ASSERT(memcmp(str, "Hello\nWorld\n\n", 13) == 0);

	free(str);
}

void test_open_temp_file(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "Line one\nLine two\nLine three\n", 29);
	close(fd);

	struct buffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(0, rc);
	TEST_ASSERT_EQUAL_INT(4, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Line one", (char *)buf->row[0].chars);
	TEST_ASSERT_EQUAL_STRING("Line two", (char *)buf->row[1].chars);
	TEST_ASSERT_EQUAL_STRING("Line three", (char *)buf->row[2].chars);

	unlink(tmpname);
}

void test_open_empty_file(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	close(fd);

	struct buffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(0, rc);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);

	unlink(tmpname);
}

/* Regression: fopen(dir, "r") succeeds on Linux, so editorOpen used
 * to present a directory as an empty, editable buffer. */
void test_open_directory_fails(void) {
	char tmpname[] = "/tmp/emil_test_dir_XXXXXX";
	TEST_ASSERT_NOT_NULL(mkdtemp(tmpname));

	struct buffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(-1, rc);
	TEST_ASSERT_NULL(buf->filename);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);

	rmdir(tmpname);
}

/* Regression: a nonexistent path with a trailing '/' names a
 * directory; it used to be offered as a "new file" that could never
 * be saved. */
void test_open_trailing_slash_fails(void) {
	struct buffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, "/tmp/emil_no_such_dir_xyzzy/");
	TEST_ASSERT_EQUAL_INT(-1, rc);
	TEST_ASSERT_NULL(buf->filename);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
}

/* ---- UTF-8 validation ---- */

void test_utf8_valid_file(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "Hello \xC2\xA2 \xE2\x82\xAC\n", 13);
	close(fd);

	struct buffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(0, rc);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);

	unlink(tmpname);
}

void test_utf8_invalid_continuation(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "Bad \xC2\x41\n", 7);
	close(fd);

	struct buffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(-1, rc);

	unlink(tmpname);
}

void test_utf8_overlong_rejected(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "\xC0\xAF\n", 3);
	close(fd);

	struct buffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(-1, rc);

	unlink(tmpname);
}

void test_utf8_null_byte_rejected(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	const char data[] = "AB\x00"
			    "CD\n";
	write(fd, data, 6);
	close(fd);

	struct buffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(-1, rc);

	unlink(tmpname);
}

void test_utf8_truncated_multibyte(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "A\xE2\x82\n", 4);
	close(fd);

	struct buffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(-1, rc);

	unlink(tmpname);
}

/* ---- save() UTF-8 guard ----
 *
 * Every load path refuses files that fail UTF-8 validation, so
 * writing an invalid buffer would produce a file emil itself cannot
 * reopen.  save() must refuse outright with an error message, leave
 * the file untouched, and not mark the buffer clean. */

void test_save_valid_utf8_succeeds(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	close(fd);

	const char *lines[] = { "hello \xE6\x97\xA5" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	buf->filename = xstrdup(tmpname);
	buf->dirty = 1;

	save();

	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "Wrote"));
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	/* buf is displaced from E.headbuf by make_test_buffer below;
	 * destroy it first so the harness cleanup sees no leak. */
	destroyBuffer(buf);

	/* Round trip: what we saved must reopen */
	struct buffer *buf2 = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf2, tmpname));

	unlink(tmpname);
}

void test_save_invalid_utf8_refused(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "old", 3);
	close(fd);

	const char *lines[] = { "abc" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	buf->filename = xstrdup(tmpname);
	buf->dirty = 1;
	/* Corrupt the row in place: 0xC2 with no continuation byte */
	buf->row[0].chars[1] = 0xC2;

	save();

	/* Refused with an error... */
	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "Save failed"));
	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "invalid UTF-8"));
	/* ...the buffer is still dirty... */
	TEST_ASSERT_EQUAL_INT(1, buf->dirty);
	/* ...and the on-disk file is untouched */
	FILE *fp = fopen(tmpname, "rb");
	TEST_ASSERT_NOT_NULL(fp);
	if (fp) {
		char content[16];
		size_t n = fread(content, 1, sizeof(content), fp);
		fclose(fp);
		TEST_ASSERT_EQUAL_INT(3, (int)n);
		TEST_ASSERT(memcmp(content, "old", 3) == 0);
	}

	unlink(tmpname);
}

void setUp(void) {
	initTestEditor();
}void tearDown(void) {
	cleanupTestEditor();
}

/* ---- Final-newline invariant ----
 *
 * A file buffer ends in a newline: its last row is empty unless the
 * buffer is empty.  Established here, at load, so that save() has no
 * policy to apply and never modifies the buffer on the way out.
 *
 * These pin the two ends of that: what a file without a trailing
 * newline becomes on load, and what it costs (nothing) if the user
 * never edits it. */

static struct buffer *openBytes(const char *bytes, size_t n, char *tmpname) {
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	if (n > 0)
		TEST_ASSERT_EQUAL_INT((int)n, (int)write(fd, bytes, n));
	close(fd);
	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, tmpname));
	return buf;
}

static void assertSerialises(struct buffer *buf, const char *want) {
	size_t len = 0;
	char *got = rowsToString(buf, &len);
	TEST_ASSERT_EQUAL_INT((int)strlen(want), (int)len);
	if (len == strlen(want))
		TEST_ASSERT_EQUAL_INT(0, memcmp(want, got, len));
	free(got);
}

/* A file that already ends in a newline is unchanged by the rule. */
void test_load_keeps_existing_final_newline(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	struct buffer *buf = openBytes("a\nb\n", 4, tmpname);

	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("", (char *)buf->row[2].chars);
	assertSerialises(buf, "a\nb\n");

	unlink(tmpname);
}

/* A file without one gains it at load, not at save. */
void test_load_adds_missing_final_newline(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	struct buffer *buf = openBytes("a\nb", 3, tmpname);

	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("b", (char *)buf->row[1].chars);
	TEST_ASSERT_EQUAL_STRING("", (char *)buf->row[2].chars);
	assertSerialises(buf, "a\nb\n");

	unlink(tmpname);
}

/* And gaining it does not mark the buffer modified, so an untouched
 * file is not rewritten, prompts nothing on quit, and keeps its mtime.
 * appendRowRaw deliberately leaves the dirty flag alone; this is the
 * property that makes the load-time rule free. */
void test_load_adding_final_newline_leaves_buffer_clean(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	struct buffer *buf = openBytes("no trailing newline", 19, tmpname);

	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
	TEST_ASSERT_NULL(buf->undo);

	unlink(tmpname);
}

/* An empty file stays empty: it has no lines, so it has nothing to
 * terminate, and it must not grow a newline out of nowhere. */
void test_load_empty_file_serialises_to_nothing(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	struct buffer *buf = openBytes("", 0, tmpname);

	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
	assertSerialises(buf, "");

	unlink(tmpname);
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_getline_short);
	RUN_TEST(test_getline_exact_120);
	RUN_TEST(test_getline_long);
	RUN_TEST(test_getline_no_newline);
	RUN_TEST(test_getline_empty);
	RUN_TEST(test_getline_multiple_reallocs);

	RUN_TEST(test_rows_to_string);
	RUN_TEST(test_open_temp_file);
	RUN_TEST(test_open_empty_file);
	RUN_TEST(test_open_directory_fails);
	RUN_TEST(test_open_trailing_slash_fails);

	RUN_TEST(test_utf8_valid_file);
	RUN_TEST(test_utf8_invalid_continuation);
	RUN_TEST(test_utf8_overlong_rejected);
	RUN_TEST(test_utf8_null_byte_rejected);
	RUN_TEST(test_utf8_truncated_multibyte);

	RUN_TEST(test_save_valid_utf8_succeeds);
	RUN_TEST(test_save_invalid_utf8_refused);

	RUN_TEST(test_load_keeps_existing_final_newline);
	RUN_TEST(test_load_adds_missing_final_newline);
	RUN_TEST(test_load_adding_final_newline_leaves_buffer_clean);
	RUN_TEST(test_load_empty_file_serialises_to_nothing);

	return TEST_END();
}
