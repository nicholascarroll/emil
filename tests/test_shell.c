/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
/* test_shell.c: Shell commands and diff-buffer-with-file.
 *
 * M-! (shell-command), M-| (shell-command-on-region), C-u M-| (replace
 * the region with the output) and M-x diff-buffer-with-file are run
 * the way a user runs them: the command token goes through
 * processKeypress(), the command line is typed at the prompt it
 * opens, and the test reads the buffer the editor ends up showing.
 * Nothing here copies the editor's own logic, so a break in the shell
 * commands turns a test red.
 *
 * The three C-g tests at the end are the exception.  They need an
 * interrupt source the test controls, which only
 * pipeCommandCaptureIntr() offers short of a real terminal. */

#include "test.h"
#include "test_harness.h"

#ifndef EMIL_DISABLE_SHELL

#include "emil_subprocess.h"
#include "emil.h"
#include "buffer.h"
#include "fileio.h"
#include "keymap.h"
#include "pipe.h"
#include "util.h"
#include <fcntl.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <time.h>



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

/* ---- Watchdog ------------------------------------------------------
 * alarm() shares ITIMER_REAL with fileio.c's
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

/* ---- Helper: issue a command and answer the prompt it opens ----
 * `typed` and RET are waiting when `cmd` is dispatched.  Under the
 * watchdog, so a command that never comes back fails the suite
 * rather than hanging it. */
static void run_typing(int cmd, const char *typed) {
	int keys[256];
	int n = 0;
	while (typed[n] && n < 255) {
		keys[n] = (unsigned char)typed[n];
		n++;
	}
	keys[n++] = '\r';

	scriptKeys(keys, n);
	muteStdout();
	watchdogStart(300);
	processKeypress(cmd);
	watchdogStop();
	unmuteStdout();
	clearKeys();
}

/* M-! command RET */
static void shell_command(const char *command) {
	run_typing(CMD_SHELL_CMD, command);
}

/* M-| command RET, on the region between mark and point */
static void shell_command_on_region(const char *command) {
	run_typing(CMD_PIPE_CMD, command);
}

/* C-u M-| command RET: the output replaces the region */
static void shell_command_replacing_region(const char *command) {
	E.uarg = 4;
	run_typing(CMD_PIPE_CMD, command);
}

/* M-x name RET */
static void meta_x(const char *name) {
	run_typing(CMD_EXEC_CMD, name);
}

/* ---- Helper: mark at (mx, my), point at (px, py) ---- */
static void set_region(struct buffer *buf, int mx, int my, int px, int py) {
	buf->markx = mx;
	buf->marky = my;
	buf->mark_active = 1;
	buf->cx = px;
	buf->cy = py;
}

/* ---- Helper: is the buffer now in front of the user called `name`? */
static int showing(const char *name) {
	return E.buf && E.buf->filename &&
	       strcmp(E.buf->filename, name) == 0 &&
	       E.windows[0]->buf == E.buf;
}

/* The whole buffer as the bytes a save would write. */
#define ASSERT_TEXT(expected, buf)                           \
	do {                                                 \
		size_t _len;                                 \
		char *_text = rowsToString((buf), &_len);    \
		TEST_ASSERT_EQUAL_STRING((expected), _text); \
		free(_text);                                 \
	} while (0)

/* ================================================================
 * 1. Shell command (no region): run command, show its output
 * ================================================================ */

void test_shell_multiline_to_buffer(void) {
	struct buffer *buf = make_test_buffer("untouched");

	shell_command("printf 'line1\\nline2\\nline3\\n'");

	/* The output is in front of the user, in its own buffer: one
	 * row per line.  The newline that ends the last line does not
	 * become an empty row after it. */
	TEST_ASSERT(showing("*Shell Output*"));
	TEST_ASSERT(E.buf != buf);
	TEST_ASSERT_EQUAL_INT(3, E.buf->numrows);
	TEST_ASSERT_EQUAL_STRING("line1", row_str(E.buf, 0));
	TEST_ASSERT_EQUAL_STRING("line2", row_str(E.buf, 1));
	TEST_ASSERT_EQUAL_STRING("line3", row_str(E.buf, 2));

	/* The buffer the command was run from is as it was. */
	ASSERT_TEXT("untouched\n", buf);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}

void test_shell_utf8_output(void) {
	make_test_buffer("");

	/* "café" and a newline; the é is written as octal escapes so
	 * that only ASCII is typed at the prompt. */
	shell_command("printf 'caf\\303\\251\\n'");

	TEST_ASSERT(showing("*Shell Output*"));
	TEST_ASSERT_EQUAL_INT(1, E.buf->numrows);
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", row_str(E.buf, 0));
}

/* Output that is not UTF-8 is refused rather than loaded: no output
 * buffer appears and the user stays where they were. */
void test_shell_invalid_utf8_output_refused(void) {
	struct buffer *buf = make_test_buffer("untouched");

	shell_command("printf '\\377\\376\\n'");

	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT(E.windows[0]->buf == buf);
	TEST_ASSERT(strlen(E.statusmsg) > 0);
	ASSERT_TEXT("untouched\n", buf);
}

/* ================================================================
 * 2. Shell command on region: feed selected text, show the output
 * ================================================================ */

void test_shell_pipe_multiline_region(void) {
	const char *lines[] = { "banana", "apple", "cherry" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);

	/* The three lines, each with its newline. */
	set_region(buf, 0, 0, 0, 3);
	shell_command_on_region("sort");

	TEST_ASSERT(showing("*Shell Output*"));
	TEST_ASSERT_EQUAL_INT(3, E.buf->numrows);
	TEST_ASSERT_EQUAL_STRING("apple", row_str(E.buf, 0));
	TEST_ASSERT_EQUAL_STRING("banana", row_str(E.buf, 1));
	TEST_ASSERT_EQUAL_STRING("cherry", row_str(E.buf, 2));

	/* Without C-u the region itself is left alone. */
	ASSERT_TEXT("banana\napple\ncherry\n", buf);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}

/* ================================================================
 * 3. Shell command replacing region: C-u M-|
 * ================================================================ */

void test_shell_pipe_region(void) {
	struct buffer *buf = make_test_buffer("hello world");

	/* Only "world" is selected. */
	set_region(buf, 6, 0, 11, 0);
	shell_command_replacing_region("tr a-z A-Z");

	/* The output went into the buffer in place of the region, and
	 * no output buffer was opened. */
	TEST_ASSERT(E.buf == buf);
	ASSERT_TEXT("hello WORLD\n", buf);
	TEST_ASSERT_TRUE(buf->dirty);

	/* One undo takes the whole replacement back. */
	processKeypress(CMD_UNDO);
	ASSERT_TEXT("hello world\n", buf);
}

/* ================================================================
 * 4. Diff buffer with file
 * ================================================================ */

/* Whether `diff` can be spawned at all.  Measured once, not assumed:
 * the Redox image does not carry it, and diffBufferWithFile() has a
 * documented answer for that case which is worth asserting rather than
 * stepping around. */
static int diffAvailable(void) {
	static int cached = -1;
	if (cached >= 0)
		return cached;

	const char *probe[] = { "diff", "--version", NULL };
	struct subprocess_s proc;
	if (subprocess_create(probe,
			      subprocess_option_inherit_environment |
				      subprocess_option_search_user_path,
			      &proc) != 0) {
		cached = 0;
		return cached;
	}
	int ec;
	subprocess_join(&proc, &ec);
	subprocess_destroy(&proc);
	cached = 1;
	return cached;
}

void test_diff_no_filename(void) {
	struct buffer *buf = make_test_buffer("some content");

	meta_x("diff-buffer-with-file");

	/* Nothing to compare with: the user is told so and no diff
	 * buffer is opened. */
	TEST_ASSERT(strlen(E.statusmsg) > 0);
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT(E.windows[0]->buf == buf);
	ASSERT_TEXT("some content\n", buf);
}

void test_diff_identical(void) {
	const char *content = "line one\nline two\n";
	char *path = write_temp_file(content);
	if (!path) { TEST_ASSERT_NOT_NULL(path); return; }

	const char *lines[] = { "line one", "line two" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	E.buf->filename = xstrdup(path);
	E.buf->dirty = 1;

	meta_x("diff-buffer-with-file");

	/* Both outcomes are asserted, so this cannot pass by the status
	 * line merely being non-empty -- which is what it used to check,
	 * and why it passed on Redox while proving nothing. */
	if (diffAvailable())
		TEST_ASSERT_EQUAL_STRING("No differences", E.statusmsg);
	else
		TEST_ASSERT_EQUAL_STRING("Diff failed: cannot create subprocess",
					 E.statusmsg);

	/* Either way no diff buffer is opened. */
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT(E.windows[0]->buf == buf);

	unlink(path);
	free(path);
}

/* The file says "old line", the buffer says "new line": the command
 * opens a read-only *Diff* buffer holding diff's output.
 *
 * This needs diff to be installed.  The Redox image does not carry
 * it, and the editor has an answer for that case ("Diff failed:
 * cannot create subprocess"), so that answer is asserted there
 * instead, as in test_diff_identical. */
void test_command_output_becomes_buffer_rows(void) {
	char *path = write_temp_file("old line\n");
	if (!path) { TEST_ASSERT_NOT_NULL(path); return; }

	struct buffer *buf = make_test_buffer("new line");
	E.buf->filename = xstrdup(path);
	E.buf->dirty = 1;

	meta_x("diff-buffer-with-file");

	if (!diffAvailable()) {
		TEST_ASSERT_EQUAL_STRING("Diff failed: cannot create subprocess",
					 E.statusmsg);
		TEST_ASSERT(E.buf == buf);
		unlink(path);
		free(path);
		return;
	}

	TEST_ASSERT(showing("*Diff*"));
	TEST_ASSERT(E.buf != buf);
	TEST_ASSERT_TRUE(E.buf->read_only);

	/* One line removed, one added, each on a row of its own. */
	int found_old = 0, found_new = 0;
	for (int i = 0; i < E.buf->numrows; i++) {
		const char *r = row_str(E.buf, i);
		if (strcmp(r, "-old line") == 0)
			found_old++;
		if (strcmp(r, "+new line") == 0)
			found_new++;
	}
	TEST_ASSERT_EQUAL_INT(1, found_old);
	TEST_ASSERT_EQUAL_INT(1, found_new);

	/* The buffer that was compared is as it was. */
	ASSERT_TEXT("new line\n", buf);

	unlink(path);
	free(path);
}

#endif /* EMIL_DISABLE_SHELL */

/* ---- Main ---- */

void setUp(void) {
	initTestEditor();
	makeMinibuffer();
	setupCommands();
}
void tearDown(void) {
	freeMinibuffer();
	cleanupTestEditor();
}


/* ---- Pipe deadlock regressions ------------------------------------ */
/* Output, input, or both larger than a pipe holds (~64 KB).  Each
 * runs under the watchdog in run_typing(), so a regressed deadlock is
 * a failure, not a hang. */

void test_pipe_large_output_no_deadlock(void) {
	make_test_buffer("");

	shell_command("seq 1 60000");

	/* All 60000 lines arrived, in order.  Their digits come to
	 * 288894 bytes (9 + 180 + 2700 + 36000 + 250005), far more than
	 * a pipe holds. */
	TEST_ASSERT(showing("*Shell Output*"));
	TEST_ASSERT_EQUAL_INT(60000, E.buf->numrows);
	TEST_ASSERT_EQUAL_STRING("1", row_str(E.buf, 0));
	TEST_ASSERT_EQUAL_STRING("60000", row_str(E.buf, 59999));
	size_t digits = 0;
	for (int i = 0; i < E.buf->numrows; i++)
		digits += (size_t)E.buf->row[i].size;
	TEST_ASSERT_EQUAL_UINT(288894, digits);
}

/* A buffer holding one line of n 'x' characters, all of it selected. */
static char *make_big_region(struct buffer **out, size_t n) {
	char *in = xmalloc(n + 1);
	memset(in, 'x', n);
	in[n] = 0;
	struct buffer *buf = make_test_buffer(in);
	set_region(buf, 0, 0, (int)n, 0);
	*out = buf;
	return in;
}

void test_pipe_large_region_roundtrip_no_deadlock(void) {
	size_t n = 200 * 1024;
	struct buffer *buf;
	char *in = make_big_region(&buf, n);

	shell_command_on_region("cat");

	/* cat hands back exactly what it was sent. */
	TEST_ASSERT(showing("*Shell Output*"));
	TEST_ASSERT(E.buf != buf);
	ASSERT_TEXT(in, E.buf);
	free(in);
}

void test_pipe_child_exits_early(void) {
	/* main.c ignores SIGPIPE; match that so writes to an exited
	 * child fail with EPIPE instead of killing the test. */
	signal(SIGPIPE, SIG_IGN);
	size_t n = 200 * 1024;
	struct buffer *buf;
	char *in = make_big_region(&buf, n);

	/* dd, not `head -c 10`: -c is a GNU/BSD extension and POSIX head
	 * takes only -n, so a strictly conforming head rejects it, writes
	 * nothing, and the assertion below fails for a reason that has
	 * nothing to do with the pipe.  Caught on WASIX against sbase
	 * (§1.2 commits emil to POSIX.1-2001, so the suite should not
	 * assume extensions either).  What the test needs is a child that
	 * takes 10 bytes and exits while the parent is still writing. */
	shell_command_on_region("dd bs=1 count=10 2>/dev/null");

	TEST_ASSERT(showing("*Shell Output*"));
	TEST_ASSERT(E.buf != buf);
	ASSERT_TEXT("xxxxxxxxxx", E.buf);
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
	/* While a shell command runs the editor watches the terminal,
	 * fd 0, for C-g.  Give the suite an fd 0 with nothing on it, so
	 * that it neither reads its caller's input nor changes the
	 * settings of the terminal it was started from. */
	int devnull = open("/dev/null", O_RDONLY);
	if (devnull >= 0) {
		dup2(devnull, STDIN_FILENO);
		if (devnull != STDIN_FILENO)
			close(devnull);
	}

	/* 1. Shell command, no region */
	RUN_TEST(test_shell_multiline_to_buffer);
	RUN_TEST(test_shell_utf8_output);
	RUN_TEST(test_shell_invalid_utf8_output_refused);

	RUN_TEST(test_pipe_large_output_no_deadlock);
	RUN_TEST(test_pipe_large_region_roundtrip_no_deadlock);
	RUN_TEST(test_pipe_child_exits_early);
	RUN_TEST(test_pipe_cancel_ctrl_g);
	RUN_TEST(test_pipe_cancel_escalates_to_sigkill);
	RUN_TEST(test_pipe_intr_ignores_other_bytes);

	/* 2. Shell command on region */
	RUN_TEST(test_shell_pipe_multiline_region);

	/* 3. Shell command replacing region */
	RUN_TEST(test_shell_pipe_region);

	/* 4. Diff buffer with file */
	RUN_TEST(test_diff_no_filename);
	RUN_TEST(test_diff_identical);
	RUN_TEST(test_command_output_becomes_buffer_rows);
#endif

	return TEST_END();
}
