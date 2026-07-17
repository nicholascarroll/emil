/* test_shell.c — Integration tests for pipe.c shell operations.
 *
 * Tests the four user-facing operations that use subprocess:
 *
 *   1. Shell command (no region) — run command, capture output
 *   2. Shell command piping region — feed selected text, capture output
 *   3. Shell command replacing region — feed region, replace in buffer
 *   4. Diff buffer with file — compare dirty buffer against saved file
 *
 * Operations 1-3 go through the static transformerPipeCmd, which we
 * can't call directly.  We replicate the same subprocess + row-splitting
 * integration that pipeCmd performs.
 *
 * Operation 4 tests the early-return guards by calling diffBufferWithFile()
 * directly (no refreshScreen on those paths), and tests diff output via
 * run_command() which uses the same subprocess flags as diffBufferWithFile.
 *
 * Section 0 ("platform diagnostics") verifies each prerequisite so that
 * failures on any platform pinpoint the exact broken assumption rather
 * than cascading into opaque NULLs in later tests. */

#include "test.h"
#include "test_harness.h"

#ifndef EMIL_DISABLE_SHELL

#include "emil_subprocess.h"
#include "buffer.h"
#include "display.h"
#include "fileio.h"
#include "pipe.h"
#include "region.h"
#include "unicode.h"
#include "util.h"
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

extern struct config E;

/* ---- Helper: portable temp directory ---- */
static const char *get_tmpdir(void) {
	const char *d = getenv("TMPDIR");
	if (d && *d) return d;
	d = getenv("TMP");
	if (d && *d) return d;
	d = getenv("TEMP");
	if (d && *d) return d;
	return "/tmp";
}

/* ---- Helper: build a mkstemp template in the portable tmpdir.
 *      Caller frees the returned string. ---- */
static char *make_tmp_template(const char *prefix) {
	const char *dir = get_tmpdir();
	size_t dlen = strlen(dir);
	size_t plen = strlen(prefix);
	/* dir + "/" + prefix + "XXXXXX" + NUL */
	char *tmpl = xmalloc(dlen + 1 + plen + 6 + 1);
	memcpy(tmpl, dir, dlen);
	tmpl[dlen] = '/';
	memcpy(tmpl + dlen + 1, prefix, plen);
	memcpy(tmpl + dlen + 1 + plen, "XXXXXX", 7); /* includes NUL */
	return tmpl;
}

/* ---- Helper: read all of stdout from a subprocess into a malloc'd
 *      string.  Returns NULL on alloc failure. ---- */
static char *read_stdout(FILE *p_stdout) {
	int bsiz = BUFSIZ + 1;
	char *buf = calloc(1, bsiz);
	if (!buf)
		return NULL;
	int c = fgetc(p_stdout);
	int i = 0;
	while (c != EOF) {
		buf[i++] = c;
		buf[i] = 0;
		if (i >= bsiz - 10) {
			bsiz <<= 1;
			buf = realloc(buf, bsiz);
		}
		c = fgetc(p_stdout);
	}
	return buf;
}

/* ---- Helper: replicate the subprocess + read pattern from
 *      transformerPipeCmd, returning a malloc'd string.
 *      Returns NULL on failure (caller must handle). ---- */
static char *run_shell(const char *shell_cmd, const char *input) {
	const char *command_line[4] = { "/bin/sh", "-c", shell_cmd, NULL };
	struct subprocess_s proc;
	int rc = subprocess_create(command_line,
				   subprocess_option_inherit_environment,
				   &proc);
	if (rc != 0) {
		fprintf(stderr, "    run_shell: subprocess_create failed for '%s'\n",
			shell_cmd);
		return NULL;
	}

	FILE *p_stdin = subprocess_stdin(&proc);
	FILE *p_stdout = subprocess_stdout(&proc);

	if (input && p_stdin) {
		for (int i = 0; input[i]; i++)
			fputc(input[i], p_stdin);
	}

	int sub_ret;
	if (subprocess_join(&proc, &sub_ret) != 0) {
		fprintf(stderr, "    run_shell: subprocess_join failed for '%s'\n",
			shell_cmd);
		subprocess_destroy(&proc);
		return NULL;
	}

	if (!p_stdout) {
		fprintf(stderr, "    run_shell: stdout is NULL for '%s'\n",
			shell_cmd);
		subprocess_destroy(&proc);
		return NULL;
	}

	char *buf = read_stdout(p_stdout);
	subprocess_destroy(&proc);
	return buf;
}

/* ---- Helper: resolve a command name to an absolute path by
 *      searching PATH (like posix_spawnp, but explicit).
 *      Returns a malloc'd string, or NULL if not found. ---- */
static char *resolve_in_path(const char *name) {
	if (name[0] == '/') {
		if (access(name, X_OK) == 0)
			return xstrdup(name);
		return NULL;
	}

	const char *path_env = getenv("PATH");
	if (!path_env) return NULL;

	char *path_copy = xstrdup(path_env);
	char *saveptr = NULL;
	char *dir = strtok_r(path_copy, ":", &saveptr);
	char trial[1024];

	while (dir) {
		int n = snprintf(trial, sizeof(trial), "%s/%s", dir, name);
		if (n > 0 && (size_t)n < sizeof(trial)) {
			if (access(trial, X_OK) == 0) {
				free(path_copy);
				return xstrdup(trial);
			}
		}
		dir = strtok_r(NULL, ":", &saveptr);
	}
	free(path_copy);
	return NULL;
}

/* ---- Helper: run a direct command with PATH search (no shell).
 *      This matches how diffBufferWithFile spawns diff.
 *      Returns NULL on failure (caller must handle). ---- */
static char *run_command(const char *const argv[], int *exit_code) {
	struct subprocess_s proc;
	int rc = subprocess_create(argv,
				   subprocess_option_inherit_environment |
				   subprocess_option_search_user_path,
				   &proc);
	if (rc != 0) {
		fprintf(stderr, "    run_command: subprocess_create failed for '%s'"
			" (rc=%d, errno=%d: %s)\n",
			argv[0], rc, errno, strerror(errno));
		return NULL;
	}

	int sub_ret = -1;
	if (subprocess_join(&proc, &sub_ret) != 0) {
		fprintf(stderr, "    run_command: subprocess_join failed for '%s'\n",
			argv[0]);
		subprocess_destroy(&proc);
		return NULL;
	}
	if (exit_code)
		*exit_code = sub_ret;

	FILE *p_stdout = subprocess_stdout(&proc);
	if (!p_stdout) {
		fprintf(stderr, "    run_command: stdout is NULL for '%s'\n",
			argv[0]);
		subprocess_destroy(&proc);
		return NULL;
	}

	char *buf = read_stdout(p_stdout);
	subprocess_destroy(&proc);
	return buf;
}

/* ---- Helper: replicate the row-splitting logic from pipeCmd,
 *      creating a buffer from shell output. ---- */
static struct buffer *output_to_buffer(const char *output) {
	struct buffer *buf = newBuffer();
	buf->filename = xstrdup("*Shell Output*");
	buf->special_buffer = 1;
	size_t len = strlen(output);
	size_t rowStart = 0;
	size_t rowLen = 0;
	for (size_t i = 0; i < len; i++) {
		if (output[i] == '\n') {
			insertRow(buf, buf->numrows,
				  (const uint8_t *)&output[rowStart], rowLen);
			rowStart = i + 1;
			rowLen = 0;
		} else {
			rowLen++;
			if (i == len - 1) {
				insertRow(buf, buf->numrows,
					  (const uint8_t *)&output[rowStart], rowLen);
			}
		}
	}
	return buf;
}

/* ---- Helper: write a string to a temporary file, return path. ---- */
static char *write_temp_file(const char *content) {
	char *path = make_tmp_template("emil-test-");
	int fd = mkstemp(path);
	if (fd == -1) {
		fprintf(stderr, "    write_temp_file: mkstemp failed for '%s': %s\n",
			path, strerror(errno));
		free(path);
		return NULL;
	}
	size_t len = strlen(content);
	size_t total = 0;
	while (total < len) {
		ssize_t n = write(fd, content + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "    write_temp_file: write failed: %s\n",
				strerror(errno));
			close(fd);
			unlink(path);
			free(path);
			return NULL;
		}
		total += n;
	}
	close(fd);
	return path;
}

/* ================================================================
 * 0. Platform diagnostics — baby-step verification of prerequisites
 *
 * Each test isolates ONE assumption.  When something breaks on a
 * new platform, the first failing diagnostic identifies the cause.
 * ================================================================ */

/* 0a. Can we get a temp directory and create a file in it? */
void test_platform_tmpdir_writable(void) {
	make_test_buffer("");
	const char *td = get_tmpdir();
	fprintf(stderr, "    tmpdir = '%s'\n", td);
	TEST_ASSERT_NOT_NULL(td);
	TEST_ASSERT(strlen(td) > 0);

	char *path = write_temp_file("hello\n");
	if (!path) { TEST_ASSERT_NOT_NULL(path); return; }
	fprintf(stderr, "    temp file = '%s'\n", path);

	FILE *f = fopen(path, "r");
	TEST_ASSERT_NOT_NULL(f);
	if (f) {
		char buf[32] = {0};
		char *got = fgets(buf, sizeof(buf), f);
		fclose(f);
		TEST_ASSERT_NOT_NULL(got);
		TEST_ASSERT_EQUAL_STRING("hello\n", buf);
	}
	unlink(path);
	free(path);
}

/* 0b. Does /bin/sh exist and can posix_spawn run it? */
void test_platform_bin_sh_exists(void) {
	make_test_buffer("");
	int sh_ok = (access("/bin/sh", X_OK) == 0);
	fprintf(stderr, "    /bin/sh accessible: %s\n", sh_ok ? "yes" : "no");
	TEST_ASSERT(sh_ok);
}

/* 0c. Can subprocess_create + join work for /bin/sh -c 'echo test'? */
void test_platform_subprocess_basic(void) {
	make_test_buffer("");
	char *output = run_shell("echo subprocess_ok", NULL);
	if (!output) {
		fprintf(stderr, "    subprocess basic echo FAILED\n");
		TEST_ASSERT_NOT_NULL(output);
		return;
	}
	fprintf(stderr, "    subprocess basic echo: ok\n");
	TEST_ASSERT_EQUAL_STRING("subprocess_ok\n", output);
	free(output);
}

/* 0d. Is `diff` findable via PATH? */
void test_platform_diff_in_path(void) {
	make_test_buffer("");
	char *diff_path = resolve_in_path("diff");
	if (diff_path) {
		fprintf(stderr, "    diff found: '%s'\n", diff_path);
		TEST_ASSERT(access(diff_path, X_OK) == 0);
		free(diff_path);
	} else {
		fprintf(stderr, "    diff NOT found in PATH\n");
		const char *p = getenv("PATH");
		fprintf(stderr, "    PATH = '%s'\n", p ? p : "(null)");
		TEST_ASSERT_NOT_NULL(diff_path); /* deliberate fail */
	}
}

/* 0e. Can posix_spawnp actually launch `diff`?
 *     Uses self-diff (exit 0) since --version is not portable
 *     (BusyBox and OpenIndiana diff don't support it). */
void test_platform_spawnp_diff(void) {
	make_test_buffer("");

	char *path = write_temp_file("spawnp test\n");
	if (!path) { TEST_ASSERT_NOT_NULL(path); return; }

	const char *argv[] = { "diff", path, path, NULL };
	int exit_code = -1;
	char *output = run_command(argv, &exit_code);
	if (!output) {
		fprintf(stderr, "    posix_spawnp('diff') FAILED to launch\n");
		char *abs = resolve_in_path("diff");
		fprintf(stderr, "    resolve_in_path: %s\n",
			abs ? abs : "NOT FOUND");
		free(abs);
		TEST_ASSERT_NOT_NULL(output);
		unlink(path); free(path);
		return;
	}
	fprintf(stderr, "    posix_spawnp('diff') ok, exit=%d\n", exit_code);
	TEST_ASSERT_EQUAL_INT(0, exit_code);

	unlink(path);
	free(path);
	free(output);
}

/* 0f. End-to-end: diff two different temp files */
void test_platform_diff_two_files(void) {
	make_test_buffer("");
	char *f1 = write_temp_file("aaa\n");
	char *f2 = write_temp_file("bbb\n");
	if (!f1 || !f2) {
		fprintf(stderr, "    temp file creation failed\n");
		TEST_ASSERT_NOT_NULL(f1);
		TEST_ASSERT_NOT_NULL(f2);
		if (f1) { unlink(f1); free(f1); }
		if (f2) { unlink(f2); free(f2); }
		return;
	}

	const char *argv[] = { "diff", "-u", f1, f2, NULL };
	int exit_code = -1;
	char *output = run_command(argv, &exit_code);
	if (!output) {
		fprintf(stderr, "    diff of two temp files FAILED\n");
		fprintf(stderr, "    file1='%s' file2='%s'\n", f1, f2);
		TEST_ASSERT_NOT_NULL(output);
		unlink(f1); unlink(f2); free(f1); free(f2);
		return;
	}

	/* diff returns 1 when files differ */
	TEST_ASSERT_EQUAL_INT(1, exit_code);
	TEST_ASSERT(strlen(output) > 0);
	TEST_ASSERT(strstr(output, "aaa") != NULL);
	TEST_ASSERT(strstr(output, "bbb") != NULL);

	unlink(f1); unlink(f2);
	free(f1); free(f2); free(output);
}

/* ================================================================
 * 1. Shell command (no region) — run command, capture output
 * ================================================================ */

void test_shell_echo_to_buffer(void) {
	make_test_buffer("");
	char *output = run_shell("echo hello world", NULL);
	if (!output) { TEST_ASSERT_NOT_NULL(output); return; }
	TEST_ASSERT_EQUAL_STRING("hello world\n", output);

	struct buffer *outbuf = output_to_buffer(output);
	TEST_ASSERT_EQUAL_INT(1, outbuf->numrows);
	TEST_ASSERT_EQUAL_STRING("hello world", row_str(outbuf, 0));

	E.headbuf->next = outbuf;
	free(output);
}

void test_shell_multiline_to_buffer(void) {
	make_test_buffer("");
	char *output = run_shell("printf 'line1\\nline2\\nline3\\n'", NULL);
	if (!output) { TEST_ASSERT_NOT_NULL(output); return; }

	struct buffer *outbuf = output_to_buffer(output);
	TEST_ASSERT_EQUAL_INT(3, outbuf->numrows);
	TEST_ASSERT_EQUAL_STRING("line1", row_str(outbuf, 0));
	TEST_ASSERT_EQUAL_STRING("line2", row_str(outbuf, 1));
	TEST_ASSERT_EQUAL_STRING("line3", row_str(outbuf, 2));

	E.headbuf->next = outbuf;
	free(output);
}

void test_shell_pipeline_to_buffer(void) {
	make_test_buffer("");
	char *output = run_shell("echo hello | tr h H", NULL);
	if (!output) { TEST_ASSERT_NOT_NULL(output); return; }

	struct buffer *outbuf = output_to_buffer(output);
	TEST_ASSERT_EQUAL_INT(1, outbuf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(outbuf, 0));

	E.headbuf->next = outbuf;
	free(output);
}

void test_shell_utf8_output(void) {
	make_test_buffer("");
	const char *utf8_input = "caf\xc3\xa9\n";
	char *output = run_shell("cat", utf8_input);
	if (!output) { TEST_ASSERT_NOT_NULL(output); return; }

	int valid = utf8_validate((uint8_t *)output, (int)strlen(output));
	TEST_ASSERT(valid);

	struct buffer *outbuf = output_to_buffer(output);
	TEST_ASSERT_EQUAL_INT(1, outbuf->numrows);
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", row_str(outbuf, 0));

	E.headbuf->next = outbuf;
	free(output);
}

/* ================================================================
 * 2. Shell command piping region — feed selected text, capture output
 * ================================================================ */

void test_shell_pipe_region(void) {
	make_test_buffer("");
	char *output = run_shell("tr a-z A-Z", "hello world");
	if (!output) { TEST_ASSERT_NOT_NULL(output); return; }
	TEST_ASSERT_EQUAL_STRING("HELLO WORLD", output);
	free(output);
}

void test_shell_pipe_multiline_region(void) {
	make_test_buffer("");
	char *output = run_shell("sort", "banana\napple\ncherry\n");
	if (!output) { TEST_ASSERT_NOT_NULL(output); return; }

	struct buffer *outbuf = output_to_buffer(output);
	TEST_ASSERT_EQUAL_INT(3, outbuf->numrows);
	TEST_ASSERT_EQUAL_STRING("apple", row_str(outbuf, 0));
	TEST_ASSERT_EQUAL_STRING("banana", row_str(outbuf, 1));
	TEST_ASSERT_EQUAL_STRING("cherry", row_str(outbuf, 2));

	E.headbuf->next = outbuf;
	free(output);
}

/* ================================================================
 * 3. Shell command replacing region — feed region, replace in buffer
 * ================================================================ */

void test_shell_region_replacement_text(void) {
	make_test_buffer("hello world");
	char *output = run_shell("tr a-z A-Z", "hello world");
	if (!output) { TEST_ASSERT_NOT_NULL(output); return; }

	struct buffer *outbuf = output_to_buffer(output);
	TEST_ASSERT_EQUAL_INT(1, outbuf->numrows);
	TEST_ASSERT_EQUAL_STRING("HELLO WORLD", row_str(outbuf, 0));

	E.headbuf->next = outbuf;
	free(output);
}

void test_shell_region_replace_multiline(void) {
	const char *lines[] = { "3", "1", "2" };
	make_test_buffer_lines(lines, 3);

	char *output = run_shell("sort -n", "3\n1\n2\n");
	if (!output) { TEST_ASSERT_NOT_NULL(output); return; }

	struct buffer *outbuf = output_to_buffer(output);
	TEST_ASSERT_EQUAL_INT(3, outbuf->numrows);
	TEST_ASSERT_EQUAL_STRING("1", row_str(outbuf, 0));
	TEST_ASSERT_EQUAL_STRING("2", row_str(outbuf, 1));
	TEST_ASSERT_EQUAL_STRING("3", row_str(outbuf, 2));

	E.headbuf->next = outbuf;
	free(output);
}

/* ================================================================
 * 4. Diff buffer with file
 *
 * test_diff_no_filename and test_diff_not_dirty call
 * diffBufferWithFile() directly — these hit early-return paths
 * that never reach refreshScreen.
 *
 * test_diff_identical also calls diffBufferWithFile() directly —
 * diff returns 0 so it returns before refreshScreen.
 *
 * test_diff_shows_differences and test_diff_multiline_changes
 * use run_command() with the exact same argv and flags that
 * diffBufferWithFile uses internally, then verify the output
 * parsing.  This avoids refreshScreen while testing the real
 * subprocess+diff integration.
 * ================================================================ */

void test_diff_no_filename(void) {
	make_test_buffer("some content");
	diffBufferWithFile();
	TEST_ASSERT(strlen(E.statusmsg) > 0);
}

void test_diff_not_dirty(void) {
	make_test_buffer("some content");
	E.buf->filename = make_tmp_template("emil-noexist-");
	E.buf->dirty = 0;
	diffBufferWithFile();
	TEST_ASSERT(strlen(E.statusmsg) > 0);
}

void test_diff_identical(void) {
	const char *content = "line one\nline two\n";
	char *path = write_temp_file(content);
	if (!path) { TEST_ASSERT_NOT_NULL(path); return; }

	const char *lines[] = { "line one", "line two" };
	make_test_buffer_lines(lines, 2);
	E.buf->filename = xstrdup(path);
	E.buf->dirty = 1;

	diffBufferWithFile();

	TEST_ASSERT(strlen(E.statusmsg) > 0);
	TEST_ASSERT_NULL(E.headbuf->next);

	unlink(path);
	free(path);
}

void test_diff_shows_differences(void) {
	make_test_buffer("");

	char *path = write_temp_file("old line\n");
	if (!path) { TEST_ASSERT_NOT_NULL(path); return; }

	char *buf_path = write_temp_file("new line\n");
	if (!buf_path) {
		TEST_ASSERT_NOT_NULL(buf_path);
		unlink(path); free(path);
		return;
	}

	const char *diff_argv[] = { "diff", "-u", path, buf_path, NULL };
	int diff_exit;
	char *output = run_command(diff_argv, &diff_exit);
	if (!output) {
		TEST_ASSERT_NOT_NULL(output);
		unlink(path); unlink(buf_path); free(path); free(buf_path);
		return;
	}

	TEST_ASSERT_EQUAL_INT(1, diff_exit);
	TEST_ASSERT(strlen(output) > 0);

	struct buffer *diff_buf = output_to_buffer(output);
	TEST_ASSERT(diff_buf->numrows > 0);

	int found_old = 0, found_new = 0;
	for (int i = 0; i < diff_buf->numrows; i++) {
		const char *r = row_str(diff_buf, i);
		if (strstr(r, "old line"))
			found_old = 1;
		if (strstr(r, "new line"))
			found_new = 1;
	}
	TEST_ASSERT(found_old);
	TEST_ASSERT(found_new);

	E.headbuf->next = diff_buf;
	unlink(path);
	unlink(buf_path);
	free(path);
	free(buf_path);
	free(output);
}

void test_diff_multiline_changes(void) {
	make_test_buffer("");

	char *path = write_temp_file("alpha\nbeta\ngamma\n");
	if (!path) { TEST_ASSERT_NOT_NULL(path); return; }

	char *buf_path = write_temp_file("alpha\nCHANGED\ngamma\n");
	if (!buf_path) {
		TEST_ASSERT_NOT_NULL(buf_path);
		unlink(path); free(path);
		return;
	}

	const char *diff_argv[] = { "diff", "-u", path, buf_path, NULL };
	int diff_exit;
	char *output = run_command(diff_argv, &diff_exit);
	if (!output) {
		TEST_ASSERT_NOT_NULL(output);
		unlink(path); unlink(buf_path); free(path); free(buf_path);
		return;
	}

	TEST_ASSERT_EQUAL_INT(1, diff_exit);

	struct buffer *diff_buf = output_to_buffer(output);
	TEST_ASSERT(diff_buf->numrows > 0);

	int found_beta = 0, found_changed = 0;
	for (int i = 0; i < diff_buf->numrows; i++) {
		const char *r = row_str(diff_buf, i);
		if (strstr(r, "beta"))
			found_beta = 1;
		if (strstr(r, "CHANGED"))
			found_changed = 1;
	}
	TEST_ASSERT(found_beta);
	TEST_ASSERT(found_changed);

	E.headbuf->next = diff_buf;
	unlink(path);
	unlink(buf_path);
	free(path);
	free(buf_path);
	free(output);
}

#endif /* EMIL_DISABLE_SHELL */

/* ---- Main ---- */

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}


/* ---- Pipe deadlock regressions ------------------------------------ */
/* The old transformerPipeCmd wrote all stdin, joined, then read
 * stdout; diffBufferWithFile joined before reading.  Both deadlocked
 * permanently once either pipe filled (~64 KB).
 *
 * Watchdog note: alarm() shares ITIMER_REAL with fileio.c's
 * timed_stat/timed_lockFile machinery, and the harness installs
 * fileCheckAlarm as the SIGALRM handler — which only sets a flag and
 * would NOT terminate a deadlocked test (the pump loop retries on
 * EINTR).  So the watchdog temporarily restores the default SIGALRM
 * disposition, which kills the test binary and turns a regressed
 * deadlock into a suite FAILURE instead of a hang, then reinstalls
 * the harness handler afterwards. */

static void (*watchdog_saved_handler)(int);

static void watchdogStart(unsigned sec) {
	watchdog_saved_handler = signal(SIGALRM, SIG_DFL);
	alarm(sec);
}

static void watchdogStop(void) {
	alarm(0);
	signal(SIGALRM, watchdog_saved_handler);
}

void test_pipe_large_output_no_deadlock(void) {
	watchdogStart(300);
	uint8_t *r = pipeCommandCapture((const uint8_t *)"seq 1 60000", NULL);
	watchdogStop();
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_TRUE(strlen((char *)r) > 65536);
	free(r);
}

void test_pipe_large_region_roundtrip_no_deadlock(void) {
	size_t n = 200 * 1024;
	uint8_t *in = xmalloc(n + 1);
	memset(in, 'x', n);
	in[n] = 0;
	watchdogStart(300);
	uint8_t *r = pipeCommandCapture((const uint8_t *)"cat", in);
	watchdogStop();
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_INT((int)n, (int)strlen((char *)r));
	free(r);
	free(in);
}

void test_pipe_child_exits_early(void) {
	/* main.c ignores SIGPIPE; match that so writes to an exited
	 * child fail with EPIPE instead of killing the test. */
	signal(SIGPIPE, SIG_IGN);
	size_t n = 200 * 1024;
	uint8_t *in = xmalloc(n + 1);
	memset(in, 'x', n);
	in[n] = 0;
	watchdogStart(300);
	uint8_t *r = pipeCommandCapture((const uint8_t *)"head -c 10", in);
	watchdogStop();
	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_INT(10, (int)strlen((char *)r));
	free(r);
	free(in);
}

/* ---- C-g cancellation ---- */

/* Preloading the interrupt pipe with C-g before the call makes the
 * pump cancel on its first select() — no threads or tty needed. */
void test_pipe_cancel_ctrl_g(void) {
	int intr[2];
	TEST_ASSERT_EQUAL_INT(0, pipe(intr));
	uint8_t cg = 0x07;
	TEST_ASSERT_EQUAL_INT(1, (int)write(intr[1], &cg, 1));

	int canceled = -1;
	watchdogStart(300);
	uint8_t *r = pipeCommandCaptureIntr((const uint8_t *)"sleep 30",
					    NULL, intr[0], &canceled);
	watchdogStop();

	TEST_ASSERT_NULL(r);
	TEST_ASSERT_EQUAL_INT(1, canceled);
	close(intr[0]);
	close(intr[1]);
}

/* A SIGINT-ignoring child must still die via the second C-g
 * (SIGKILL).  Escalation is user-driven — no timers — so the test
 * simply delivers two C-g presses from a helper process, spaced so
 * the first arrives after the child's 'trap' is installed and the
 * second demonstrates that SIGINT alone was not enough:
 *
 *   t=1s  C-g #1 -> SIGINT   (ignored by the trap)
 *   t=2s  C-g #2 -> SIGKILL  (unignorable)
 *
 * A correct run therefore finishes in ~2 s.  If SIGKILL were
 * broken, the child would sleep out its full 20 s, which the
 * elapsed-time bound catches long before the watchdog. */
void test_pipe_cancel_escalates_to_sigkill(void) {
	/* First C-g after 2 s: ample margin for the target's spawn
	 * and trap-install even on emulated runners; second 1 s
	 * later. */
	FILE *cg_helper = popen(
		"sleep 2; printf '\\007'; sleep 1; printf '\\007'", "r");
	TEST_ASSERT_NOT_NULL(cg_helper);

	int canceled = -1;
	time_t t0 = time(NULL);
	watchdogStart(300);
	uint8_t *r = pipeCommandCaptureIntr(
		(const uint8_t *)"trap '' INT; sleep 20", NULL,
		fileno(cg_helper), &canceled);
	watchdogStop();
	time_t t1 = time(NULL);

	TEST_ASSERT_NULL(r);
	TEST_ASSERT_EQUAL_INT(1, canceled);
	/* Discriminator: sleep durations are wall-clock even on
	 * slow emulated CI (QEMU RISC-V/PowerPC), so a working
	 * SIGKILL path finishes in ~3 s plus spawn overhead, while a
	 * broken one waits out the child's full 20 s sleep.  15 s
	 * leaves headroom for emulated process-spawn overhead while
	 * staying safely under the 20 s floor of the failure mode.
	 * Watchdog budgets in these tests are hang detectors, not
	 * performance bounds — hence 300 s. */
	TEST_ASSERT_TRUE(t1 - t0 <= 15);
	pclose(cg_helper);
}

/* Non-C-g type-ahead on the interrupt fd is discarded and does not
 * cancel the command. */
void test_pipe_intr_ignores_other_bytes(void) {
	int intr[2];
	TEST_ASSERT_EQUAL_INT(0, pipe(intr));
	TEST_ASSERT_EQUAL_INT(3, (int)write(intr[1], "abc", 3));

	int canceled = -1;
	watchdogStart(300);
	uint8_t *r = pipeCommandCaptureIntr((const uint8_t *)"echo hi",
					    NULL, intr[0], &canceled);
	watchdogStop();

	TEST_ASSERT_NOT_NULL(r);
	TEST_ASSERT_EQUAL_INT(0, canceled);
	TEST_ASSERT_EQUAL_STRING("hi\n", (char *)r);
	free(r);
	close(intr[0]);
	close(intr[1]);
}

int main(void) {
	TEST_BEGIN();

#ifndef EMIL_DISABLE_SHELL
	/* 0. Platform diagnostics */
	RUN_TEST(test_platform_tmpdir_writable);
	RUN_TEST(test_platform_bin_sh_exists);
	RUN_TEST(test_platform_subprocess_basic);
	RUN_TEST(test_platform_diff_in_path);
	RUN_TEST(test_platform_spawnp_diff);
	RUN_TEST(test_platform_diff_two_files);

	/* 1. Shell command, no region */
	RUN_TEST(test_shell_echo_to_buffer);
	RUN_TEST(test_shell_multiline_to_buffer);
	RUN_TEST(test_shell_pipeline_to_buffer);
	RUN_TEST(test_shell_utf8_output);

	RUN_TEST(test_pipe_large_output_no_deadlock);
	RUN_TEST(test_pipe_large_region_roundtrip_no_deadlock);
	RUN_TEST(test_pipe_child_exits_early);
	RUN_TEST(test_pipe_cancel_ctrl_g);
	RUN_TEST(test_pipe_cancel_escalates_to_sigkill);
	RUN_TEST(test_pipe_intr_ignores_other_bytes);

	/* 2. Shell command piping region */
	RUN_TEST(test_shell_pipe_region);
	RUN_TEST(test_shell_pipe_multiline_region);

	/* 3. Shell command replacing region */
	RUN_TEST(test_shell_region_replacement_text);
	RUN_TEST(test_shell_region_replace_multiline);

	/* 4. Diff buffer with file */
	RUN_TEST(test_diff_no_filename);
	RUN_TEST(test_diff_not_dirty);
	RUN_TEST(test_diff_identical);
	RUN_TEST(test_diff_shows_differences);
	RUN_TEST(test_diff_multiline_changes);
#endif

	return TEST_END();
}
