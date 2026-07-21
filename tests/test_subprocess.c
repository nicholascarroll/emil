/* test_subprocess.c: Contract tests for the subprocess API surface
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

/* ---- Watchdog: alarm() with default disposition so a hung test
 * kills the binary (suite FAIL) instead of wedging CI.  The harness
 * installs a flag-only SIGALRM handler which would otherwise
 * swallow the alarm. ---- */

#include <signal.h>
#include <sys/select.h>
#include <unistd.h>

static void (*wd_saved)(int);
static void wdStart(unsigned sec) {
	wd_saved = signal(SIGALRM, SIG_DFL);
	alarm(sec);
}
static void wdStop(void) {
	alarm(0);
	signal(SIGALRM, wd_saved);
}

/* ---- Cancellation contract: subprocess_signal ---- */

/* Signalling a spawned child terminates it and join reaps it. */
void test_subprocess_signal_terminates(void) {
	const char *cmd[] = { "/bin/sh", "-c", "sleep 30", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);

	wdStart(300);
	TEST_ASSERT_EQUAL_INT(0, subprocess_signal(&proc, SIGTERM));
	int exit_code;
	TEST_ASSERT_EQUAL_INT(0, subprocess_join(&proc, &exit_code));
	wdStop();

	subprocess_destroy(&proc);
}

/* Signalling reaches the WHOLE pipeline (process group), not just
 * /bin/sh: 'cat' holds our stdout pipe open, so stdout only reaches
 * EOF if cat itself died.  Under an ungrouped kill, cat would live
 * for the full 30 s and this test would be killed by the watchdog. */
void test_subprocess_signal_kills_pipeline(void) {
	/* The right side announces readiness so we never signal
	 * before the pipeline exists (signalling too early kills sh
	 * pre-fork and the test proves nothing). */
	const char *cmd[] = { "/bin/sh", "-c",
			      "sleep 30 | { echo ready; cat; }", NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(cmd, subprocess_option_inherit_environment,
				   &proc);
	TEST_ASSERT_EQUAL_INT(0, rc);
	if (!proc.grouped) {
		/* Platform couldn't spawn a process group: graceful
		 * degradation.  Group semantics can't hold, so only
		 * verify the immediate child dies. */
		wdStart(300);
		subprocess_signal(&proc, SIGTERM);
		int ec;
		TEST_ASSERT_EQUAL_INT(0, subprocess_join(&proc, &ec));
		wdStop();
		subprocess_destroy(&proc);
		return;
	}

	wdStart(300);

	/* Wait for "ready\n" — the stdout-holding child now exists. */
	{
		char rb[16] = { 0 };
		size_t got = 0;
		int rfd = fileno(subprocess_stdout(&proc));
		while (got < 6) {
			ssize_t n = read(rfd, rb + got, 6 - got);
			if (n <= 0)
				break;
			got += (size_t)n;
		}
		TEST_ASSERT_EQUAL_STRING("ready\n", rb);
	}

	TEST_ASSERT_EQUAL_INT(0, subprocess_signal(&proc, SIGTERM));

	/* stdout must reach EOF promptly — proves cat died too. */
	int fd = fileno(subprocess_stdout(&proc));
	int got_eof = 0;
	for (int i = 0; i < 50 && !got_eof; i++) { /* <= 5 s */
		fd_set r;
		struct timeval tv = { 0, 100000 };
		FD_ZERO(&r);
		FD_SET(fd, &r);
		if (select(fd + 1, &r, NULL, NULL, &tv) > 0) {
			char b[256];
			ssize_t n = read(fd, b, sizeof(b));
			if (n <= 0)
				got_eof = 1;
		}
	}
	TEST_ASSERT_TRUE(got_eof);

	int exit_code;
	TEST_ASSERT_EQUAL_INT(0, subprocess_join(&proc, &exit_code));
	wdStop();
	subprocess_destroy(&proc);
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
	RUN_TEST(test_subprocess_signal_terminates);
	RUN_TEST(test_subprocess_signal_kills_pipeline);
#endif

	return TEST_END();
}
