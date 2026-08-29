/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* pty_harness.h: shared machinery for the two pty integration tests.
 *
 * Drives the real emil binary under a pseudo-terminal: allocate a pty,
 * spawn the editor on the slave, write bytes to the master, read the
 * frames back.  C only, no dependencies beyond POSIX (_XOPEN_SOURCE 600
 * for posix_openpt and friends).
 *
 * There are two tests because there are two different requirements:
 *
 *   pty_input_test    needs a pty.  Runs wherever posix_openpt works.
 *   pty_signals_test  needs a pty whose master reports the slave's
 *                     termios, and real job control.  Linux and macOS.
 *
 * That split is the whole of the platform handling.  A scenario either
 * runs here or it is not in this binary; there is no skip, no advisory
 * mode and no environment variable that turns assertions off.  Which
 * binaries get built is decided once, in tests/run_tests.sh.
 *
 * Both are driven by run_tests.sh at the end of `make test`.  Exit
 * status is the number of failing assertions.
 */

#ifndef PTY_HARNESS_H
#define PTY_HARNESS_H


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

static inline void capReset(void) {
	cap_len = 0;
}

/* Multiplier applied to every settle window (see pump).  Every wait in
 * this file is "give the editor time to produce its output", so
 * stretching them can only reduce flakiness, never change what is
 * asserted.  Needed where the editor is slower to respond than a
 * native build on an idle machine: under a Wasm runtime, under an
 * emulator, or on a loaded CI runner.  Set EMIL_PTY_TIME_SCALE=4 (or
 * whatever) rather than editing the constants, so the native timings
 * stay honest about how fast the editor actually is.
 *
 * Note this deliberately does NOT scale the deliberate *input* gaps
 * (the 400ms split-sequence pause), which are part of what those
 * scenarios assert rather than slack for the editor to use. */
static inline int timeScale(void) {
	static int scale = 0;
	if (scale == 0) {
		const char *s = getenv("EMIL_PTY_TIME_SCALE");
		scale = s ? atoi(s) : 1;
		if (scale < 1)
			scale = 1;
	}
	return scale;
}

/* Collect output from the pty master for duration_ms, appending to
 * the capture buffer. */
static inline void pump(int fd, int duration_ms) {
	struct pollfd pfd;
	int waited = 0;
	duration_ms *= timeScale();
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
static inline const char *stripped(void) {
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

static inline int contains(const char *haystack, const char *needle) {
	return strstr(haystack, needle) != NULL;
}

/* ---- child management ----------------------------------------- */

struct child {
	pid_t pid;
	int mfd;
};

/* Spawn emil on a fresh pty of the given size, optionally opening a
 * file.  Returns 0 on success, -1 if no pty is available (caller
 * should SKIP), exits on setup bugs. */
static inline int spawnEmilOpts(struct child *c, const char *file, int cols,
			 int rows) {
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
		ws.ws_row = (unsigned short)rows;
		ws.ws_col = (unsigned short)cols;
		ws.ws_xpixel = 0;
		ws.ws_ypixel = 0;
		ioctl(sfd, TIOCSWINSZ, &ws);
		dup2(sfd, STDIN_FILENO);
		dup2(sfd, STDOUT_FILENO);
		dup2(sfd, STDERR_FILENO);
		if (sfd > STDERR_FILENO)
			close(sfd);
		close(mfd);
		if (file != NULL)
			execl(emil_path, emil_path, file, (char *)NULL);
		else
			execl(emil_path, emil_path, (char *)NULL);
		_exit(127);
	}

	c->pid = pid;
	c->mfd = mfd;
	capReset();
	pump(mfd, 700); /* first paint */
	return 0;
}

/* Spawn emil on a fresh 24x80 pty with no file: what every scenario
 * written before the frame-truncation one below wants. */
static inline int spawnEmil(struct child *c) {
	return spawnEmilOpts(c, NULL, 80, 24);
}

/* Named sendBytes, not send: send(2) is a POSIX socket function, and
 * a static shadowing it compiles only for as long as no header in the
 * include chain happens to declare it.  That held here by luck until a
 * feature-test macro pulled in <sys/socket.h> on Darwin and the
 * collision became a hard error.  Kept renamed now that the macro is
 * gone, because the hazard was always latent. */
static inline void sendBytes(struct child *c, const char *bytes, size_t n,
		      int settle_ms) {
	ssize_t w = write(c->mfd, bytes, n);
	(void)w;
	pump(c->mfd, settle_ms);
}

static inline void sendStr(struct child *c, const char *s, int settle_ms) {
	sendBytes(c, s, strlen(s), settle_ms);
}


/* Send text the way a terminal delivers a paste: one write, so the
 * bytes are already buffered when the drain loop looks, and CR as the
 * line separator.
 *
 * CR is not a detail.  A terminal sends \r for a line break -- that is
 * what the Enter key transmits in raw mode -- and keymap.c maps it to
 * CMD_NEWLINE.  A harness that sends \n instead is testing input no
 * terminal produces, and will pass while multi-line paste is visibly
 * broken.  Every paste scenario below uses this helper for that
 * reason. */
static inline void sendPaste(struct child *c, const char *text, int settle_ms) {
	sendBytes(c, text, strlen(text), settle_ms);
}

static inline int childAlive(struct child *c) {
	return waitpid(c->pid, NULL, WNOHANG) == 0;
}

/* Running, stopped, or gone.
 *
 * childAlive() cannot tell the first two apart -- waitpid() without
 * WUNTRACED reports a stopped child as simply not exited -- and for the
 * suspend scenarios that difference is the whole question: a stopped
 * editor is *supposed* to have handed the terminal back. */
enum childState { CHILD_RUNNING, CHILD_STOPPED, CHILD_GONE };

static inline enum childState childState(struct child *c) {
	int st;
	pid_t r = waitpid(c->pid, &st, WNOHANG | WUNTRACED);
	if (r == 0)
		return CHILD_RUNNING;
	if (r == c->pid && WIFSTOPPED(st))
		return CHILD_STOPPED;
	return CHILD_GONE;
}

static inline void reap(struct child *c) {
	kill(c->pid, SIGKILL);
	waitpid(c->pid, NULL, 0);
	close(c->mfd);
}

/* ---- scenario bookkeeping ------------------------------------- */

static int scenario_failures;
static int total_failures;
static const char *current_name;

static inline void begin(const char *name) {
	current_name = name;
	scenario_failures = 0;
}

static inline void expect(int condition, const char *what) {
	if (!condition) {
		scenario_failures++;
		total_failures++;
		printf("      %s\n", what);
	}
}

static inline void finish(void) {
	printf("  %-44s %s\n", current_name,
	       scenario_failures ? "FAIL" : "PASS");
}


/* ---- entry-point boilerplate
 ----------------------------------- */

/* Resolve the editor path and confirm the platform can host the test.
 * Returns 0 when it cannot, having said why; the caller returns 0 and
 * run_tests.sh reports a run of zero scenarios as a failure. */
static inline int ptyBegin(const char *prog, int argc, char **argv) {
	emil_path = (argc > 1) ? argv[1] : "./emil";
	signal(SIGPIPE, SIG_IGN);

	if (access(emil_path, X_OK) != 0) {
		printf("%s: cannot execute %s\n", prog, emil_path);
		return 0;
	}
	int probe = posix_openpt(O_RDWR | O_NOCTTY);
	if (probe == -1) {
		printf("%s: no pty available: %s\n", prog, strerror(errno));
		return 0;
	}
	close(probe);
	printf("%s: driving %s\n", prog, emil_path);
	return 1;
}

static inline int ptyEnd(const char *prog) {
	if (total_failures)
		printf("%s: %d assertion(s) failed\n", prog, total_failures);
	else
		printf("%s: all scenarios passed\n", prog);
	return total_failures;
}

#endif /* PTY_HARNESS_H */
