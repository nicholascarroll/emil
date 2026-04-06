/* test_subprocess.c — Contract tests for the subprocess API surface
 * that pipe.c depends on.
 *
 * These tests define the behaviour any replacement for subprocess.h
 * must satisfy.  They cover exactly the two patterns pipe.c uses:
 *
 *   Pattern 1 (transformerPipeCmd): /bin/sh -c with optional stdin,
 *     join, read stdout, destroy.
 *   Pattern 2 (diffBufferWithFile): direct command with PATH search,
 *     no stdin, join, read stdout, destroy.
 *
 * Both patterns read stdout AFTER subprocess_join. */

#include "test.h"
#include "test_harness.h"

#ifndef EMIL_DISABLE_SHELL

#include "emil_subprocess.h"
#include <string.h>
#include <stdlib.h>

/* ---- Pattern 2: direct command, no stdin, read after join ---- */

void test_subprocess_echo(void) {
	const char *cmd[] = { "/bin/echo", "hello", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	int exit_code;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT_EQUAL_INT(0, exit_code);

	FILE *out = subprocess_stdout(&proc);
	TEST_ASSERT_NOT_NULL(out);
	char buf[64] = { 0 };
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING("hello\n", buf);

	subprocess_destroy(&proc);
}

void test_subprocess_exit_code(void) {
	const char *cmd[] = { "/bin/sh", "-c", "exit 42", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	int exit_code;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT_EQUAL_INT(42, exit_code);

	subprocess_destroy(&proc);
}

/* ---- Pattern 1: /bin/sh -c, stdin piped, read stdout after join ---- */

void test_subprocess_stdin_stdout(void) {
	const char *cmd[] = { "/bin/cat", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	FILE *in = subprocess_stdin(&proc);
	fprintf(in, "hello world\n");
	fflush(in);

	int exit_code;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT_EQUAL_INT(0, exit_code);

	FILE *out = subprocess_stdout(&proc);
	char buf[64] = { 0 };
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING("hello world\n", buf);

	subprocess_destroy(&proc);
}

void test_subprocess_multiline_stdin(void) {
	const char *cmd[] = { "/bin/cat", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	FILE *in = subprocess_stdin(&proc);
	fprintf(in, "line1\nline2\nline3\n");
	fflush(in);

	int exit_code;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT_EQUAL_INT(0, exit_code);

	FILE *out = subprocess_stdout(&proc);
	char buf[64] = { 0 };
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING("line1\n", buf);
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING("line2\n", buf);
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING("line3\n", buf);

	subprocess_destroy(&proc);
}

/* ---- Shell pipeline via /bin/sh -c (how transformerPipeCmd works) ---- */

void test_subprocess_shell_pipeline(void) {
	const char *cmd[] = { "/bin/sh", "-c", "echo hello | tr h H", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	int exit_code;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT_EQUAL_INT(0, exit_code);

	FILE *out = subprocess_stdout(&proc);
	char buf[64] = { 0 };
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING("Hello\n", buf);

	subprocess_destroy(&proc);
}

void test_subprocess_shell_with_stdin(void) {
	const char *cmd[] = { "/bin/sh", "-c", "tr a-z A-Z", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	FILE *in = subprocess_stdin(&proc);
	fprintf(in, "hello\n");
	fflush(in);

	int exit_code;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT_EQUAL_INT(0, exit_code);

	FILE *out = subprocess_stdout(&proc);
	char buf[64] = { 0 };
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING("HELLO\n", buf);

	subprocess_destroy(&proc);
}

/* ---- PATH search (how diffBufferWithFile works) ---- */

void test_subprocess_path_search(void) {
	const char *cmd[] = { "echo", "found", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd,
				   subprocess_option_inherit_environment |
					   subprocess_option_search_user_path,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	int exit_code;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT_EQUAL_INT(0, exit_code);

	FILE *out = subprocess_stdout(&proc);
	char buf[64] = { 0 };
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING("found\n", buf);

	subprocess_destroy(&proc);
}

/* ---- Non-zero exit with output (pipe.c continues reading) ---- */

void test_subprocess_nonzero_exit_with_output(void) {
	const char *cmd[] = { "/bin/sh", "-c",
			      "echo partial; exit 1", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	int exit_code;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT_EQUAL_INT(1, exit_code);

	FILE *out = subprocess_stdout(&proc);
	char buf[64] = { 0 };
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING("partial\n", buf);

	subprocess_destroy(&proc);
}

/* ---- Invalid command ---- */

void test_subprocess_bad_command(void) {
	const char *cmd[] = { "/nonexistent/binary", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	/* Some implementations fail at create time, others fork
	 * successfully and fail at exec — accept either. */
	if (rc != 0)
		return; /* failed at create — fine */
	int exit_code = 0;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT(exit_code != 0);
	subprocess_destroy(&proc);
}

/* ---- UTF-8 round-trip through pipe ---- */

void test_subprocess_utf8_roundtrip(void) {
	const char *cmd[] = { "/bin/cat", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	FILE *in = subprocess_stdin(&proc);
	/* Split strings to avoid hex escape ambiguity on some compilers */
	const char *utf8 = "caf\xc3\xa9" " r\xc3\xa9" "sum\xc3\xa9" "\n";
	fprintf(in, "%s", utf8);
	fflush(in);

	int exit_code;
	subprocess_join(&proc, &exit_code);
	TEST_ASSERT_EQUAL_INT(0, exit_code);

	FILE *out = subprocess_stdout(&proc);
	char buf[128] = { 0 };
	fgets(buf, sizeof(buf), out);
	TEST_ASSERT_EQUAL_STRING(utf8, buf);

	subprocess_destroy(&proc);
}

#endif /* EMIL_DISABLE_SHELL */

/* ---- Main ---- */

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

int main(void) {
	TEST_BEGIN();

#ifndef EMIL_DISABLE_SHELL
	RUN_TEST(test_subprocess_echo);
	RUN_TEST(test_subprocess_exit_code);
	RUN_TEST(test_subprocess_stdin_stdout);
	RUN_TEST(test_subprocess_multiline_stdin);
	RUN_TEST(test_subprocess_shell_pipeline);
	RUN_TEST(test_subprocess_shell_with_stdin);
	RUN_TEST(test_subprocess_path_search);
	RUN_TEST(test_subprocess_nonzero_exit_with_output);
	RUN_TEST(test_subprocess_bad_command);
	RUN_TEST(test_subprocess_utf8_roundtrip);
#endif

	return TEST_END();
}
