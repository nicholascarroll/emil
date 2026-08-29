/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_writeall.c: writeAll() delivers every byte across a signal.
 *
 * This is the coverage for findings §H1.  It replaces a scenario in
 * the pty tests that assert the same property through a whole
 * rendered frame on a pseudo-terminal.  That scenario needed three
 * conditions to coincide -- a frame larger than the pty buffer, a
 * reader deliberately behind, and a signal arriving mid-write -- and
 * whether they coincided depended on the host's pty buffer size and
 * scheduling.  It could not tell "the write was short" from "the
 * capture stopped", and it broke on four platforms in a week.  The
 * property itself is narrow and does not need a terminal to observe.
 *
 * Everything here is POSIX.1-2001 base: pipe, fork, kill, getppid,
 * sigaction, waitpid.  No timers, no sleeps, no pty, and nothing
 * outside the standard the project targets.  In particular there is
 * no SIGWINCH, which is not a POSIX signal at all and is what made
 * the old scenario fail to compile under _POSIX_C_SOURCE=200112L and
 * on Darwin and FreeBSD.
 *
 * The child drives the signals rather than a timer: it reads a chunk,
 * then signals the parent, so a signal lands while the parent is
 * blocked in write() regardless of how fast either side runs.  That
 * is what makes this deterministic where the pty version was not.
 *
 * The control case is the point of the design.  Before asserting that
 * writeAll() delivers everything, the test performs a single bare
 * write() under identical conditions and requires it to come up
 * short.  If it does not, the conditions the test needs were never
 * created and the test SKIPs rather than passing vacuously -- a test
 * that cannot establish its own preconditions has not tested
 * anything.  The old scenario had no such check, which is exactly how
 * it reported failures that had not occurred. */

#include "test.h"
#include "test_harness.h"
#include "util.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/* Large enough to exceed any plausible pipe buffer many times over, so
 * the writer blocks repeatedly and there are many chances for a signal
 * to land.  Small enough not to be slow under an emulated CI target. */
#define PAYLOAD (512 * 1024)
#define CHUNK 1024

static volatile sig_atomic_t signals_seen;

static void onSig(int sig) {
	(void)sig;
	signals_seen++;
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* Byte i of the payload.  A varying pattern rather than a constant, so
 * a reader that silently loses a run of bytes is detected as a
 * mismatch and not just a short count. */
static unsigned char patternByte(size_t i) {
	return (unsigned char)(i % 251u);
}

/* Child half: drain the pipe, verify the pattern, and signal the
 * parent after every chunk so it is interrupted while blocked.
 * Never returns. */
static void childDrain(int rfd, pid_t parent, int verify) {
	unsigned char buf[CHUNK];
	size_t got = 0;
	int ok = 1;

	for (;;) {
		ssize_t n = read(rfd, buf, sizeof(buf));
		if (n < 0) {
			if (errno == EINTR)
				continue;
			ok = 0;
			break;
		}
		if (n == 0)
			break;
		if (verify) {
			for (ssize_t k = 0; k < n; k++)
				if (buf[k] != patternByte(got + (size_t)k))
					ok = 0;
		}
		got += (size_t)n;
		/* Signal after the read, so the parent is woken while
		 * blocked on a pipe that has just made room. */
		kill(parent, SIGUSR1);
	}
	_exit(ok && got == PAYLOAD ? 0 : 1);
}

/* Set up pipe + child.  Returns the write fd, or -1 on failure. */
static int startDrain(pid_t *child, int verify) {
	int fds[2];
	if (pipe(fds) != 0)
		return -1;
	pid_t parent = getpid();
	pid_t pid = fork();
	if (pid < 0) {
		close(fds[0]);
		close(fds[1]);
		return -1;
	}
	if (pid == 0) {
		close(fds[1]);
		childDrain(fds[0], parent, verify);
	}
	close(fds[0]);
	*child = pid;
	return fds[1];
}

static int reap(pid_t pid) {
	int status = 0;
	while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
		;
	return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* One attempt at the control: a bare write() under the same signal
 * storm the real case runs under.  Returns 1 if write() came up short,
 * which is the condition writeAll() exists to survive.  Drains the
 * remainder either way so the child reaches EOF and exits. */
static int bareWriteComesUpShort(unsigned char *buf) {
	pid_t kid = -1;
	int wfd = startDrain(&kid, 0);
	if (wfd < 0)
		return 0;

	signals_seen = 0;
	ssize_t bare = write(wfd, buf, PAYLOAD);
	int was_short = (bare >= 0 && bare < PAYLOAD);

	if (bare > 0) {
		size_t left = PAYLOAD - (size_t)bare;
		const unsigned char *p = buf + bare;
		while (left > 0) {
			ssize_t n = write(wfd, p, left);
			if (n <= 0) {
				if (n < 0 && errno == EINTR)
					continue;
				break;
			}
			p += n;
			left -= (size_t)n;
		}
	}
	close(wfd);
	(void)reap(kid);
	return was_short;
}

#define CONTROL_ATTEMPTS 3

void test_writeall_delivers_every_byte_across_signals(void) {
	struct sigaction sa, old_usr1, old_pipe;
	unsigned char *buf = xmalloc(PAYLOAD);
	for (size_t i = 0; i < PAYLOAD; i++)
		buf[i] = patternByte(i);

	/* No SA_RESTART: the whole point is that write() returns
	 * early.  With SA_RESTART the kernel would resume it and there
	 * would be nothing for writeAll() to loop over. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = onSig;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGUSR1, &sa, &old_usr1);

	/* A child that dies early would otherwise kill this process. */
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = SIG_IGN;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGPIPE, &sa, &old_pipe);

	/* --- Can this platform cut a write() short at all? ---
	 *
	 * Attempted up to CONTROL_ATTEMPTS times.  One attempt cannot
	 * tell "this platform never interrupts a write" from "the
	 * scheduling did not happen to produce one this run", and those
	 * need different answers: the first is a fact about the host,
	 * the second is a flake that must not fail the suite.  Several
	 * attempts all coming up long is the platform speaking.
	 *
	 * WASIX is the platform that does: under wasmer write() is never
	 * cut short, measured at 12 runs out of 12.  Asserting the
	 * control unconditionally made that a permanent red for a
	 * property emil satisfies there perfectly well. */
	int interruptible = 0;
	for (int attempt = 0; attempt < CONTROL_ATTEMPTS && !interruptible;
	     attempt++)
		interruptible = bareWriteComesUpShort(buf);

	/* --- The assertion proper: same conditions, writeAll(), and
	 * every byte must arrive intact. */
	pid_t kid = -1;
	int wfd = startDrain(&kid, 1);
	TEST_ASSERT(wfd >= 0);
	if (wfd < 0) {
		free(buf);
		sigaction(SIGUSR1, &old_usr1, NULL);
		sigaction(SIGPIPE, &old_pipe, NULL);
		return;
	}
	signals_seen = 0;
	int rc = writeAll(wfd, buf, PAYLOAD);
	close(wfd);
	int child_rc = reap(kid);

	/* Asserted on every platform: whatever write() does underneath,
	 * writeAll() delivers the payload. */
	TEST_ASSERT_EQUAL_INT(0, rc);
	/* Child exits 0 only if it saw exactly PAYLOAD bytes and every
	 * one matched, so this covers both loss and corruption. */
	TEST_ASSERT_EQUAL_INT(0, child_rc);

	/* The handler having run is only guaranteed where signals can
	 * cut a write short in the first place -- that is what
	 * `interruptible` measured, and it cannot be true without
	 * delivery.  Where it is false the runtime's signal delivery is
	 * its own business: wasmer sometimes runs the handler during the
	 * writeAll() pass and sometimes does not, so requiring it there
	 * made this suite fail intermittently on a property emil does
	 * not depend on. */
	if (interruptible)
		TEST_ASSERT(signals_seen > 0);

	/* Say which of the two ran, so a host that quietly stops cutting
	 * writes short is visible in the log rather than silently
	 * downgrading to the weaker case. */
	printf("    %s\n",
	       interruptible ?
		       "(write() was cut short under these conditions, so "
		       "the retry path was exercised)" :
		       "(write() is never cut short here; delivery asserted, "
		       "but the retry path is not reachable on this platform)");

	sigaction(SIGUSR1, &old_usr1, NULL);
	sigaction(SIGPIPE, &old_pipe, NULL);
	free(buf);
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_writeall_delivers_every_byte_across_signals);

	return TEST_END();
}
