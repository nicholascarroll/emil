/* test_fileio.c — File I/O, round-trip, UTF-8 validation, emil_getline. */

#include "test.h"
#include "test_harness.h"
#include "fileio.h"
#include "util.h"
#include <stdio.h>
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
	struct editorBuffer *buf = make_test_buffer(NULL);
	editorInsertRow(buf, 0, "Hello", 5);
	editorInsertRow(buf, 1, "World", 5);
	editorInsertRow(buf, 2, "", 0);

	int buflen = 0;
	char *str = editorRowsToString(buf, &buflen);
	TEST_ASSERT_EQUAL_INT(13, buflen);
	TEST_ASSERT(memcmp(str, "Hello\nWorld\n\n", 13) == 0);

	free(str);
	destroyBuffer(buf);
}

void test_open_temp_file(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "Line one\nLine two\nLine three\n", 29);
	close(fd);

	struct editorBuffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(0, rc);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Line one", (char *)buf->row[0].chars);
	TEST_ASSERT_EQUAL_STRING("Line two", (char *)buf->row[1].chars);
	TEST_ASSERT_EQUAL_STRING("Line three", (char *)buf->row[2].chars);

	destroyBuffer(buf);
	unlink(tmpname);
}

void test_open_empty_file(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	close(fd);

	struct editorBuffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(0, rc);
	TEST_ASSERT_EQUAL_INT(0, buf->numrows);

	destroyBuffer(buf);
	unlink(tmpname);
}

/* ---- UTF-8 validation ---- */

void test_utf8_valid_file(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "Hello \xC2\xA2 \xE2\x82\xAC\n", 13);
	close(fd);

	struct editorBuffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(0, rc);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);

	destroyBuffer(buf);
	unlink(tmpname);
}

void test_utf8_invalid_continuation(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "Bad \xC2\x41\n", 7);
	close(fd);

	struct editorBuffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(-1, rc);

	destroyBuffer(buf);
	unlink(tmpname);
}

void test_utf8_overlong_rejected(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "\xC0\xAF\n", 3);
	close(fd);

	struct editorBuffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(-1, rc);

	destroyBuffer(buf);
	unlink(tmpname);
}

void test_utf8_null_byte_rejected(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	const char data[] = "AB\x00" "CD\n";
	write(fd, data, 6);
	close(fd);

	struct editorBuffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(-1, rc);

	destroyBuffer(buf);
	unlink(tmpname);
}

void test_utf8_truncated_multibyte(void) {
	char tmpname[] = "/tmp/emil_test_XXXXXX";
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	write(fd, "A\xE2\x82\n", 4);
	close(fd);

	struct editorBuffer *buf = make_test_buffer(NULL);
	int rc = editorOpen(buf, tmpname);
	TEST_ASSERT_EQUAL_INT(-1, rc);

	destroyBuffer(buf);
	unlink(tmpname);
}

void setUp(void) { initTestEditor(); }
void tearDown(void) {}

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

	RUN_TEST(test_utf8_valid_file);
	RUN_TEST(test_utf8_invalid_continuation);
	RUN_TEST(test_utf8_overlong_rejected);
	RUN_TEST(test_utf8_null_byte_rejected);
	RUN_TEST(test_utf8_truncated_multibyte);

	return TEST_END();
}
