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
 * Usage: decoder_pty_test <path-to-emil>
 * Run via `make test` (wired into tests/run_tests.sh) or
 * `make test-pty`.  If no pseudo-terminal can be allocated (some
 * constrained CI environments), the whole program reports SKIP and
 * exits 0 rather than failing the suite.  Exit status is otherwise
 * the number of failing scenarios.
 */

#define _XOPEN_SOURCE 600

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
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

static void reap(struct child *c) {
	kill(c->pid, SIGKILL);
	waitpid(c->pid, NULL, 0);
	close(c->mfd);
}

/* ---- scenario bookkeeping ------------------------------------- */

static int scenario_failures;
static int total_failures;
static const char *current_name;

static void begin(const char *name) {
	current_name = name;
	scenario_failures = 0;
}

static void expect(int condition, const char *what) {
	if (!condition) {
		scenario_failures++;
		total_failures++;
		printf("      %s\n", what);
	}
}

static void finish(void) {
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

/* Alt+[ : introducer with nothing following within the timeout. */
static void scenarioAltBracket(void) {
	struct child c;
	begin("Alt+[ then x types x");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "\033[", 200);
		capReset();
		sendStr(&c, "x", 400);
		expect(contains(stripped(), "x"),
		       "x was swallowed into a CSI");
		reap(&c);
	}
	finish();
}

/* A sequence that stops mid-body times out and is reported. */
static void scenarioIncompleteRecovers(void) {
	struct child c;
	begin("incomplete \\e[1 recovers");
	if (spawnEmil(&c) == 0) {
		capReset();
		sendStr(&c, "\033[1", 400);
		expect(contains(cap, "M-[ 1"),
		       "incomplete sequence not reported");
		capReset();
		sendStr(&c, "x", 400);
		expect(contains(stripped(), "x"),
		       "keypress after recovery swallowed");
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
	scenarioAltBracket();
	scenarioIncompleteRecovers();
	scenarioMappedKeys();
	scenarioUtf8Typing();

	if (total_failures)
		printf("decoder_pty_test: %d assertion(s) failed\n",
		       total_failures);
	else
		printf("decoder_pty_test: all scenarios passed\n");
	return total_failures;
}
