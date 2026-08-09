/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* decoder_pty_test.c: terminal-level integration tests for emil's
 * escape-sequence input.  C only; no dependencies beyond POSIX
 * (_XOPEN_SOURCE 600 for posix_openpt and friends).
 *
 * Drives the real emil binary under a pseudo-terminal and asserts on
 * the rendered frames.  This covers what the unit tests in
 * test_decoder.c cannot: real timing (the Meta-prefix indefinite
 * wait versus the in-flight sequence timeout), interaction with the
 * main loop's key batching and status-message lifecycle, and the
 * historical leaked-bytes regressions (F12 panic key, SS3 finals
 * typed into the buffer, lone-ESC-then-sequence leaking its body).
 *
 * It also asserts the terminal-state invariant (4.5) directly, which
 * nothing else can: stubs.c replaces main.o for the unit suites, so
 * no unit test links a line of termios code.  Whether the editor
 * still owns the terminal is only observable from the master side of
 * a real pty.
 *
 * Usage: decoder_pty_test <path-to-emil>
 * Run via `make test` (wired into tests/run_tests.sh) or
 * `make test-pty`.  If no pseudo-terminal can be allocated (some
 * constrained CI environments), the whole program reports SKIP and
 * exits 0 rather than failing the suite.  Exit status is otherwise
 * the number of failing scenarios.
 */

#define _XOPEN_SOURCE 600
#ifdef __sun
/* Solaris/illumos hide the STREAMS declarations behind this. */
#define __EXTENSIONS__ 1
#endif

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef __sun
#include <stropts.h>
#endif
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static const char *emil_path;

/* ---- capture buffer ------------------------------------------- */

#define CAP_MAX (256 * 1024)
static char cap[CAP_MAX];
static size_t cap_len;

static void capReset(void) {
	cap_len = 0;
}

/* Collect output from the pty master for duration_ms, appending to
 * the capture buffer. */
static void pump(int fd, int duration_ms) {
	struct pollfd pfd;
	int waited = 0;
	while (waited < duration_ms) {
		pfd.fd = fd;
		pfd.events = POLLIN;
		pfd.revents = 0;
		int pr = poll(&pfd, 1, 50);
		waited += 50;
		if (pr <= 0)
			continue;
		if (!(pfd.revents & POLLIN))
			continue;
		ssize_t n = read(fd, cap + cap_len,
				 sizeof(cap) - cap_len - 1);
		if (n > 0)
			cap_len += (size_t)n;
	}
	cap[cap_len] = 0;
}

/* Copy of the capture with CSI escape sequences removed: what was
 * actually printed as text.  (ESC '[' params/intermediates then one
 * final byte in 0x40..0x7E; a lone ESC pair is skipped as two
 * bytes.) */
static char stripbuf[CAP_MAX];
static const char *stripped(void) {
	size_t o = 0;
	for (size_t i = 0; i < cap_len;) {
		unsigned char c = (unsigned char)cap[i];
		if (c != 033) {
			stripbuf[o++] = cap[i++];
			continue;
		}
		i++;
		if (i < cap_len && cap[i] == '[') {
			i++;
			while (i < cap_len &&
			       ((unsigned char)cap[i] < 0x40 ||
				(unsigned char)cap[i] > 0x7E))
				i++;
			if (i < cap_len)
				i++; /* final byte */
		} else if (i < cap_len) {
			i++; /* ESC x pair */
		}
	}
	stripbuf[o] = 0;
	return stripbuf;
}

static int contains(const char *haystack, const char *needle) {
	return strstr(haystack, needle) != NULL;
}

/* ---- child management ----------------------------------------- */

struct child {
	pid_t pid;
	int mfd;
};

/* Spawn emil on a fresh 24x80 pty.  Returns 0 on success, -1 if no
 * pty is available (caller should SKIP), exits on setup bugs. */
static int spawnEmil(struct child *c) {
	int mfd = posix_openpt(O_RDWR | O_NOCTTY);
	if (mfd == -1)
		return -1;
	if (grantpt(mfd) == -1 || unlockpt(mfd) == -1) {
		close(mfd);
		return -1;
	}
	const char *slave_name = ptsname(mfd);
	if (slave_name == NULL) {
		close(mfd);
		return -1;
	}

	pid_t pid = fork();
	if (pid == -1) {
		close(mfd);
		return -1;
	}
	if (pid == 0) {
		/* Child: new session; opening the slave without
		 * O_NOCTTY makes it the controlling terminal. */
		setsid();
		int sfd = open(slave_name, O_RDWR);
		if (sfd == -1)
			_exit(127);
#ifdef __sun
		/* On Solaris/illumos a pty is a STREAMS device and the
		 * freshly opened slave is a bare stream: no terminal
		 * semantics at all until ptem (termios ioctls, window
		 * size) and ldterm (the line discipline) are pushed
		 * onto it, in that order.  Without them every termios
		 * ioctl is unrecognized, so the stream head forwards
		 * it to the master as an M_IOCTL that nothing ever
		 * answers, and the caller blocks for the STREAMS ioctl
		 * timeout instead of failing.  Solaris 11.4 autopushes
		 * these; illumos does not, so the push is done here
		 * (I_FIND guards against a double push if the platform
		 * did it for us). */
		if (ioctl(sfd, I_FIND, "ldterm") == 0) {
			if (ioctl(sfd, I_PUSH, "ptem") == -1 ||
			    ioctl(sfd, I_PUSH, "ldterm") == -1)
				_exit(126);
			/* BSD/XENIX ioctl compatibility: not required
			 * by the editor, pushed to match what a login
			 * session's pty looks like. */
			(void)ioctl(sfd, I_PUSH, "ttcompat");
		}
#endif
		struct winsize ws;
		ws.ws_row = 24;
		ws.ws_col = 80;
		ws.ws_xpixel = 0;
		ws.ws_ypixel = 0;
		ioctl(sfd, TIOCSWINSZ, &ws);
		dup2(sfd, STDIN_FILENO);
		dup2(sfd, STDOUT_FILENO);
		dup2(sfd, STDERR_FILENO);
		if (sfd > STDERR_FILENO)
			close(sfd);
		close(mfd);
		execl(emil_path, emil_path, (char *)NULL);
		_exit(127);
	}

	c->pid = pid;
	c->mfd = mfd;
	capReset();
	pump(mfd, 700); /* first paint */
	return 0;
}

static void send(struct child *c, const char *bytes, size_t n,
		 int settle_ms) {
	ssize_t w = write(c->mfd, bytes, n);
	(void)w;
	pump(c->mfd, settle_ms);
}

static void sendStr(struct child *c, const char *s, int settle_ms) {
	send(c, s, strlen(s), settle_ms);
}

static int childAlive(struct child *c) {
	return waitpid(c->pid, NULL, WNOHANG) == 0;
}

/* Running, stopped, or gone.
 *
 * childAlive() cannot tell the first two apart -- waitpid() without
 * WUNTRACED reports a stopped child as simply not exited -- and for the
 * suspend scenarios that difference is the whole question: a stopped
 * editor is *supposed* to have handed the terminal back. */
enum childState { CHILD_RUNNING, CHILD_STOPPED, CHILD_GONE };

static enum childState childState(struct child *c) {
	int st;
	pid_t r = waitpid(c->pid, &st, WNOHANG | WUNTRACED);
	if (r == 0)
		return CHILD_RUNNING;
	if (r == c->pid && WIFSTOPPED(st))
		return CHILD_STOPPED;
	return CHILD_GONE;
}

static void reap(struct child *c) {
	kill(c->pid, SIGKILL);
	waitpid(c->pid, NULL, 0);
	close(c->mfd);
}

/* ---- scenario bookkeeping ------------------------------------- */

static int scenario_failures;
static int scenario_skipped;
static const char *scenario_skip_why;
static int total_failures;
static const char *current_name;

static void begin(const char *name) {
	current_name = name;
	scenario_failures = 0;
	scenario_skipped = 0;
	scenario_skip_why = NULL;
}

static void expect(int condition, const char *what) {
	if (!condition) {
		scenario_failures++;
		total_failures++;
		printf("      %s\n", what);
	}
}

/* Not every platform can observe what a scenario needs to observe.
 * That is a fact about the platform, not a fault in the editor, so it
 * must not be an assertion: say so and move on. */
static void skip(const char *why) {
	scenario_skipped = 1;
	scenario_skip_why = why;
}

static void finish(void) {
	if (scenario_skipped && scenario_failures == 0) {
		printf("  %-44s SKIP (%s)\n", current_name, scenario_skip_why);
		return;
	}
	printf("  %-44s %s\n", current_name,
	       scenario_failures ? "FAIL" : "PASS");
}

/* ---- scenarios ------------------------------------------------- */

/* A lone ESC before an arrow must not leak the arrow's body. */
static void scenarioLoneEscThenArrow(void) {
	struct child c;
	begin("lone ESC then Up arrow");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "line1\rline2", 200);
		sendStr(&c, "\033", 300); /* lone ESC keypress */
		sendStr(&c, "\033[A", 150);
		capReset();
		sendStr(&c, "X", 400);
		expect(contains(stripped(), "line1X"),
		       "arrow did not act as Up");
		expect(!contains(stripped(), "[A"),
		       "sequence body leaked as text");
		reap(&c);
	}
	finish();
}

/* ESC waits indefinitely: slow ESC, f must be Meta-f. */
static void scenarioMetaPrefixHumanSpeed(void) {
	struct child c;
	begin("slow ESC, f acts as M-f");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "alpha beta", 200);
		sendStr(&c, "\001", 150); /* C-a: line start */
		sendStr(&c, "\033", 400); /* human-speed Meta prefix */
		sendStr(&c, "f", 150);
		capReset();
		sendStr(&c, "X", 400);
		expect(contains(stripped(), "alphaX beta"),
		       "M-f did not move by word");
		reap(&c);
	}
	finish();
}

/* Unmapped sequences are consumed and reported, never typed. */
static void scenarioUnknownReported(const char *label,
				    const char *sequence,
				    const char *message) {
	struct child c;
	begin(label);
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "##", 200);
		capReset();
		sendStr(&c, sequence, 400);
		expect(childAlive(&c), "editor died");
		expect(contains(cap, message), "status message missing");
		capReset();
		sendStr(&c, "x", 400);
		expect(contains(stripped(), "##x"),
		       "following keypress swallowed");
		reap(&c);
	}
	finish();
}

/* Alt+[ has no binding and there is no sequence timeout, so the
 * following keystroke completes the CSI and is consumed and reported
 * rather than typed.  This is the original CSI behavior, restored
 * deliberately: correctness on split sequences is worth more than the
 * ergonomics of an unbound key. */
static void scenarioAltBracketConsumesNext(void) {
	struct child c;
	begin("Alt+[ consumes the next keystroke");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "##", 200);
		capReset();
		sendStr(&c, "\033[", 300);
		expect(!contains(cap, "M-["),
		       "reported before the sequence completed");
		capReset();
		sendStr(&c, "x", 400); /* final byte: completes the CSI */
		expect(contains(cap, "M-[ x"),
		       "completed sequence not reported");
		expect(!contains(stripped(), "##x"),
		       "final byte leaked into the buffer as text");
		capReset();
		sendStr(&c, "y", 400);
		expect(contains(stripped(), "##y"),
		       "keypress after the sequence swallowed");
		reap(&c);
	}
	finish();
}

/* THE guarantee bought by having no sequence timeout: a sequence
 * split by a slow transport (laggy link, slow serial line, TCP
 * retransmit) still decodes correctly no matter how long the gap.
 * Under any finite budget the tail of this arrow key would have been
 * typed into the buffer as the letter A.  This test fails under any
 * timed policy and passes only while sequence bytes block. */
static void scenarioSlowSplitSequence(void) {
	struct child c;
	struct timespec gap;
	begin("sequence split by 400ms still decodes");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "line1\rline2", 300);
		ssize_t w = write(c.mfd, "\033[", 2);
		(void)w;
		gap.tv_sec = 0;
		gap.tv_nsec = 400 * 1000000; /* far beyond any budget */
		nanosleep(&gap, NULL);
		w = write(c.mfd, "A", 1); /* completes the Up arrow */
		(void)w;
		pump(c.mfd, 300);
		capReset();
		sendStr(&c, "X", 400);
		expect(contains(stripped(), "line1X"),
		       "split arrow did not act as Up");
		expect(!contains(stripped(), "line2A"),
		       "sequence tail leaked into the buffer as text");
		reap(&c);
	}
	finish();
}

/* The frozen contract: representative mapped keys still act. */
static void scenarioMappedKeys(void) {
	struct child c;

	begin("CSI H Home and \\e[4~ End");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "abc", 200);
		sendStr(&c, "\033[H", 150);
		sendStr(&c, "1", 150);
		sendStr(&c, "\033[4~", 150);
		capReset();
		sendStr(&c, "2", 400);
		expect(contains(stripped(), "1abc2"),
		       "Home/End variants misdecoded");
		reap(&c);
	}
	finish();

	begin("SS3 \\eOH Home");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "one two", 200);
		sendStr(&c, "\033OH", 150);
		capReset();
		sendStr(&c, "Y", 400);
		expect(contains(stripped(), "Yone two"),
		       "SS3 Home misdecoded");
		reap(&c);
	}
	finish();

	begin("fast M-b backward word");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "word word", 200);
		sendStr(&c, "\033b", 150);
		capReset();
		sendStr(&c, "Z", 400);
		expect(contains(stripped(), "word Zword"),
		       "M-b misdecoded");
		reap(&c);
	}
	finish();
}

/* Multi-byte input is untouched by the escape path. */
static void scenarioUtf8Typing(void) {
	struct child c;
	begin("UTF-8 typing intact");
	if (spawnEmil(&c) == 0) {
		capReset();
		sendStr(&c, "\xe4\xbd\xa0\xe5\xa5\xbd", 500);
		expect(contains(cap, "\xe4\xbd\xa0\xe5\xa5\xbd"),
		       "CJK input mangled");
		reap(&c);
	}
	finish();
}

/* ---- terminal ownership (invariant 4.5) ------------------------ */

/* While the editor is running it owns the terminal in raw mode.  The
 * unit suites cannot check this at all: stubs.c replaces main.o, so
 * nothing there ever touches termios.  Only a real pty can see it.
 *
 * The suspend paths raise SIGTSTP after handing the tty back, and
 * POSIX discards a stop signal sent to a member of an orphaned
 * process group -- which is exactly what spawnEmil() creates, and
 * what an editor invoked as EDITOR/GIT_EDITOR from a daemon, a CI
 * runner or a job-control-less shell runs in.  When the stop is
 * discarded the editor keeps running, and before the fix it kept
 * running in cooked mode with ECHO and ISIG restored: the next
 * Ctrl-C killed it past the unsaved-changes prompt and past atexit.
 *
 * Reading that state from here relies on tcgetattr() on the pty
 * master reporting the slave's settings, which is a Linux/BSD
 * convenience and not portable: on illumos a pty is a STREAMS device
 * and the master is a bare stream with no terminal semantics of its
 * own, so the call simply fails.  That is a fact about the platform
 * rather than anything to do with the editor, so it is a SKIP.  The
 * end-to-end Ctrl-C scenario below needs no such visibility and
 * carries the assertion on those platforms. */
static int masterSeesSlaveTermios(struct child *c, struct termios *out) {
	if (tcgetattr(c->mfd, out) != 0)
		return 0;
	/* Succeeded, but reports ECHO on while the editor is certainly in
	 * raw mode: the master is describing itself, not the slave. */
	if (out->c_lflag & ECHO)
		return 0;
	return 1;
}

static void scenarioTerminalOwnedAfterSuspend(const char *label,
					      const char *keys) {
	struct child c;
	begin(label);
	if (spawnEmil(&c) == 0) {
		struct termios before, after;
		if (!masterSeesSlaveTermios(&c, &before)) {
			skip("master does not report slave termios");
			reap(&c);
			finish();
			return;
		}

		capReset();
		sendStr(&c, keys, 600);

		/* Whether the raise() stops the process is a property of
		 * the platform's job control, not of emil, and the
		 * correct terminal state differs between the two
		 * outcomes.  POSIX says a stop signal sent to a member
		 * of an orphaned process group -- which is what
		 * spawnEmil() creates -- is discarded, and Linux and
		 * illumos do that; Cygwin/MSYS2 stops the process
		 * anyway.  A stopped editor is *supposed* to have handed
		 * the terminal back, so asserting raw mode there would
		 * be asserting the opposite of correct behaviour. */
		switch (childState(&c)) {
		case CHILD_GONE:
			expect(0, "editor exited on suspend");
			break;

		case CHILD_STOPPED:
			/* The stop took.  Cooked mode is right, and what
			 * matters is that the editor reclaims the
			 * terminal on resume -- the same repair path,
			 * reached the ordinary way. */
			kill(c.pid, SIGCONT);
			pump(c.mfd, 600);
			if (childState(&c) != CHILD_RUNNING) {
				expect(0, "editor did not resume on SIGCONT");
				break;
			}
			if (tcgetattr(c.mfd, &after) == 0) {
				expect(!(after.c_lflag & ECHO),
				       "ECHO left on after resume");
				expect(!(after.c_lflag & ISIG),
				       "ISIG left on after resume");
				expect(!(after.c_lflag & ICANON),
				       "ICANON left on after resume");
			}
			break;

		case CHILD_RUNNING:
			/* The stop was discarded and the editor ran on,
			 * so it must still own the terminal. */
			if (tcgetattr(c.mfd, &after) == 0) {
				expect(!(after.c_lflag & ECHO),
				       "ECHO left on while the editor is running");
				expect(!(after.c_lflag & ISIG),
				       "ISIG left on: Ctrl-C would kill the editor");
				expect(!(after.c_lflag & ICANON),
				       "ICANON left on while the editor is running");
			}
			break;
		}
		reap(&c);
	}
	finish();
}

/* The consequence, asserted end to end and without needing to read any
 * termios: after a discarded suspend the editor must still survive a
 * Ctrl-C, because it must still own the terminal.  Ctrl-C is not a
 * binding -- with ISIG cleared the byte is read and reported like any
 * other -- so a live editor here is the whole assertion.  This is the
 * portable half of the pair, and on platforms where the master cannot
 * report the slave's state it is the only cover the suspend paths
 * have. */
static void scenarioCtrlCSurvivesAfterSuspend(const char *label,
					      const char *keys) {
	struct child c;
	begin(label);
	if (spawnEmil(&c) == 0) {
		sendStr(&c, keys, 600);
		switch (childState(&c)) {
		case CHILD_GONE:
			expect(0, "editor gone after suspend");
			break;
		case CHILD_STOPPED:
			/* Genuinely suspended: Ctrl-C is not delivered to
			 * a stopped process, so surviving it would prove
			 * nothing.  The termios scenario above covers
			 * this platform via the resume path instead. */
			skip("editor suspended; Ctrl-C not delivered");
			break;
		case CHILD_RUNNING:
			sendStr(&c, "\003", 600); /* Ctrl-C */
			expect(childState(&c) != CHILD_GONE,
			       "editor died on Ctrl-C after suspend");
			break;
		}
		reap(&c);
	}
	finish();
}

/* ---- main ------------------------------------------------------ */

int main(int argc, char **argv) {
	emil_path = (argc > 1) ? argv[1] : "./emil";
	signal(SIGPIPE, SIG_IGN);

	if (access(emil_path, X_OK) != 0) {
		printf("decoder_pty_test: SKIP (binary not found at %s)\n",
		       emil_path);
		return 0;
	}
	{
		/* Probe pty availability once; constrained
		 * environments skip the whole suite. */
		int probe = posix_openpt(O_RDWR | O_NOCTTY);
		if (probe == -1) {
			printf("decoder_pty_test: SKIP (no pty: %s)\n",
			       strerror(errno));
			return 0;
		}
		close(probe);
	}

	printf("decoder_pty_test: driving %s\n", emil_path);

	scenarioLoneEscThenArrow();
	scenarioMetaPrefixHumanSpeed();
	scenarioUnknownReported("unknown key reported: F5", "\033[15~",
				"M-[ 1 5 ~");
	scenarioUnknownReported("unknown key reported: Insert", "\033[2~",
				"M-[ 2 ~");
	scenarioUnknownReported("unknown key reported: Ctrl-Right",
				"\033[1;5C", "M-[ 1 ; 5 C");
	scenarioUnknownReported("unknown key reported: F1 (SS3)", "\033OP",
				"M-O P");
	scenarioAltBracketConsumesNext();
	scenarioSlowSplitSequence();
	scenarioMappedKeys();
	scenarioUtf8Typing();
	scenarioTerminalOwnedAfterSuspend("terminal owned after C-z", "\032");
	scenarioTerminalOwnedAfterSuspend("terminal owned after C-x z",
					  "\030z");
	scenarioTerminalOwnedAfterSuspend("terminal owned after C-x C-z",
					  "\030\032");
	scenarioCtrlCSurvivesAfterSuspend("Ctrl-C after C-z does not kill it",
					  "\032");
	scenarioCtrlCSurvivesAfterSuspend("Ctrl-C after C-x z does not kill it",
					  "\030z");
	scenarioCtrlCSurvivesAfterSuspend(
		"Ctrl-C after C-x C-z does not kill it", "\030\032");

	if (total_failures)
		printf("decoder_pty_test: %d assertion(s) failed\n",
		       total_failures);
	else
		printf("decoder_pty_test: all scenarios passed\n");
	return total_failures;
}
