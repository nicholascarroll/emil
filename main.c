/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "abuf.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "fileio.h"
#include "history.h"
#include "keymap.h"
#include "undo.h"

#include "terminal.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <locale.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <sys/select.h>
#include <unistd.h>
#include <wchar.h>

const int page_overlap = 2;

struct config E;
void setupHandlers(void);

/*** signal handlers (async-signal-safe) ***/

static volatile sig_atomic_t got_sigwinch = 0;
static volatile sig_atomic_t got_sigcont = 0;
static volatile sig_atomic_t got_sigterm = 0;
static volatile sig_atomic_t got_sighup = 0;

static void editorSuspend(int sig) {
	(void)sig;
	IGNORE_RETURN(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios));
	IGNORE_RETURN(write(STDOUT_FILENO, CSI "?1049l", 8));
	signal(SIGTSTP, SIG_DFL);
	raise(SIGTSTP);

	/* Reached in two cases, both meaning the editor is running
	 * again and must take the terminal back: a normal resume, or a
	 * stop that never happened.  POSIX discards a stop signal sent
	 * to a member of an orphaned process group, so raise() returns
	 * at once and no SIGCONT ever arrives -- and an orphaned group
	 * is ordinary for an editor run as EDITOR/GIT_EDITOR under a
	 * daemon, a CI runner or setsid. 
	 *
	 * handlePendingSignals() does the repair, including
	 * reinstalling the handler the SIG_DFL above just cleared. */
	got_sigcont = 1;
}

static void editorResume(int sig) {
	(void)sig;
	got_sigcont = 1;
}

/* got_sigcont is private to this file; openShellDrawer() raises
 * SIGTSTP with the handler reset to SIG_DFL, so it cannot go through
 * editorSuspend() and needs a way to ask for the same repair. */
void requestTerminalResume(void) {
	got_sigcont = 1;
}

#ifdef SIGWINCH
static void sigwinchHandler(int sig) {
	(void)sig;
	got_sigwinch = 1;
}
#endif

/* Fatal signals: hand the terminal back, then die of the same signal.
 *
 * A crash -- a real fault, or emil's own abort() from the allocation
 * guards in util.c -- otherwise leaves the tty in raw mode on the
 * alternate screen.  The shell the user drops back into then echoes
 * nothing, reads no lines and ignores Ctrl-C, and the way out is to
 * type `reset` blind.  The terminal is a persistent object: its
 * settings outlive the process that made them, which is what turns a
 * crash into a second, separate problem for the user.
 *
 * Async-signal-safe only.  tcsetattr(), write(), signal() and raise()
 * are all on POSIX's list; perror() and free() are not, which is why
 * this cannot route through die() or editorCleanup().
 * disableRawMode() is exactly one tcsetattr() and one write() -- the
 * same restore the clean exit path uses, so there is one restore and
 * not two -- and terminal.c records that this handler depends on it
 * staying that way.
 *
 * The disposition is reset before the restore so that a fault inside
 * the restore cannot re-enter this handler, and re-raised after it
 * (SA_NODEFER, so it lands inside the handler rather than on return
 * from one that must not return) so the process still dies of what
 * killed it: the shell still reports the crash and the kernel still
 * writes the core.  A handler that tidied up and exited would leave a
 * clean terminal and no evidence, which is worse than the bug.
 *
 * Not routed through the got_* flag mechanism the other handlers use:
 * those signals are ones the main loop will live to see.
 */
static void handleFatalSignal(int sig) {
	signal(sig, SIG_DFL);
	disableRawMode();
	raise(sig);
	_exit(128 + sig); /* not reached; raise() above is fatal */
}

static void handleSigterm(int sig) {
	(void)sig;
	got_sigterm = 1;
}

static void handleSighup(int sig) {
	(void)sig;
	got_sighup = 1;
}

/* Recover from a signal that its handler could only flag.
 *
 * SIGCONT (resume) and SIGWINCH (resize) both leave the editor in a
 * state that must be repaired before the next key is read, and both
 * arrive asynchronously, so the handlers do nothing but set a flag. */
void handlePendingSignals(void) {
	if (got_sigterm || got_sighup) {
		/* Checked here rather than only in the main loop, so a
		 * SIGTERM arriving while a prompt is open is acted on. */
		disableRawMode();
		_exit(1);
	}

	if (got_sigcont) {
		got_sigcont = 0;
		/* Restore terminal state after resume */
		IGNORE_RETURN(write(STDOUT_FILENO, CSI "r", 3));
		IGNORE_RETURN(write(STDOUT_FILENO, ESC "8", 2));
		setupHandlers();
		applyRawMode();
		for (int i = 0; i < E.nwindows; i++)
			E.windows[i]->height = 0;
		resizeScreen();
		resetFileCheckThrottle();
		/* resizeScreen() above already re-measured the
		 * terminal, so a resize that landed while we were
		 * stopped needs no second pass. */
		got_sigwinch = 0;
	}

	if (got_sigwinch) {
		got_sigwinch = 0;
		resizeScreen();
	}
}

/*** init ***/

void setupHandlers(void) {
#ifdef SIGWINCH
	installHandler(SIGWINCH, sigwinchHandler, 0);
#endif
	installHandler(SIGCONT, editorResume, 0);
	installHandler(SIGTSTP, editorSuspend, SA_NODEFER);
	installHandler(SIGTERM, handleSigterm, 0);
	installHandler(SIGHUP, handleSighup, 0);

	/* Installed here rather than once in main() so that the resume
	 * path re-asserts them along with the rest; re-installing an
	 * identical disposition costs nothing.  SIGBUS is XSI where
	 * SIGSEGV and SIGABRT are ISO C, so it is guarded. */
	installHandler(SIGSEGV, handleFatalSignal, SA_NODEFER);
	installHandler(SIGABRT, handleFatalSignal, SA_NODEFER);
#ifdef SIGBUS
	installHandler(SIGBUS, handleFatalSignal, SA_NODEFER);
#endif
}

void editorCleanup(void) {
	/* Free all buffers */
	struct buffer *b = E.headbuf;
	while (b) {
		struct buffer *next = b->next;
		destroyBuffer(b);
		b = next;
	}
	E.headbuf = NULL;
	E.buf = NULL;
	E.lastVisitedBuffer = NULL;

	/* Free minibuffer */
	if (E.minibuf) {
		destroyBuffer(E.minibuf);
		E.minibuf = NULL;
	}

	/* Free kill text */
	clearText(&E.kill);

	/* Free histories */
	freeHistory(&E.file_history);
	freeHistory(&E.command_history);
	freeHistory(&E.shell_history);
	freeHistory(&E.search_history);
	freeHistory(&E.replace_history);
	freeHistory(&E.rect_history);
	freeHistory(&E.kill_history);

	/* Free registers */
	for (int r = 0; r < 127; r++) {
		if (E.registers[r].rtype == REGISTER_TEXT)
			clearText(&E.registers[r].data.text);
		E.registers[r].rtype = REGISTER_NULL;
	}

	free(E.macro.keys);
	E.macro.keys = NULL;

	/* Free windows */
	for (int i = 0; i < E.nwindows; i++)
		free(E.windows[i]);
	free(E.windows);
	E.windows = NULL;
	E.nwindows = 0;

	/* Free persistent render buffer */
	abFree(&E.render_buf);
	E.render_buf.b = NULL;
}

static void initEditor(void) {
	E.statusmsg[0] = 0;
	E.kill = (struct text){ 0 };
	E.windows = xmalloc(sizeof(struct window *) * 1);
	E.windows[0] = xcalloc(1, sizeof(struct window));
	E.windows[0]->focused = 1;
	E.nwindows = 1;
	E.recording = 0;
	E.macro.nkeys = 0;
	E.macro.keys = NULL;
	E.micro = 0;
	E.playback = 0;
	E.headbuf = NULL;
	memset(E.registers, 0, sizeof(E.registers));
	setupCommands();
	E.lastVisitedBuffer = NULL;

	initHistory(&E.file_history);
	initHistory(&E.command_history);
	initHistory(&E.shell_history);
	initHistory(&E.search_history);
	initHistory(&E.replace_history);
	initHistory(&E.rect_history);
	initHistory(&E.kill_history);
	E.kill_ring_pos = -1;

	E.render_buf = (struct abuf){ NULL, 0, 0 };

	getWindowSize(&E.screenrows, &E.screencols);
}

int main(int argc, char *argv[]) {
	/*
	 * Set up a UTF-8 locale so that system wcwidth() works.
	 * Try the user's environment first, then common fallbacks.
	 */
	const char *locale_attempts[] = { "", "C.UTF-8", "en_US.UTF-8", NULL };
	for (int i = 0; locale_attempts[i] != NULL; i++) {
		if (setlocale(LC_CTYPE, locale_attempts[i]) != NULL &&
		    wcwidth((wchar_t)0x4E00) == 2)
			break;
	}

	// Check for flags before entering raw mode
	if (argc >= 2 && strncmp(argv[1], "--", 2) == 0) {
		if (strcmp(argv[1], "--version") == 0) {
			printf("emil %s\n", EMIL_VERSION);
			return 0;
		}
		fprintf(stderr, "emil: unrecognised option '%s'\n", argv[1]);
		return 1;
	}

	/*
	 * Detect piped stdin: if stdin is not a terminal, slurp the
	 * data before entering raw mode, then reopen /dev/tty.*/
	char *stdin_data = NULL;
	size_t stdin_len = 0;
	int stdin_buf_used = 0;
	if (!isatty(STDIN_FILENO)) {
		stdin_data = readAllFromFd(STDIN_FILENO, &stdin_len);

		/* Reopen /dev/tty as stdin so the terminal works */
		int tty_fd = open("/dev/tty", O_RDWR);
		if (tty_fd < 0) {
			free(stdin_data);
			fprintf(stderr, "emil: cannot open /dev/tty: %s\n",
				strerror(errno));
			exit(1);
		}
		dup2(tty_fd, STDIN_FILENO);
		if (tty_fd != STDIN_FILENO)
			close(tty_fd);
	}

	enableRawMode();
	initEditor();
	atexit(editorCleanup);
	setupHandlers();
	signal(SIGPIPE, SIG_IGN);
	signal(SIGUSR1, SIG_IGN);
	signal(SIGUSR2, SIG_IGN);
	initFileCheck();

	E.headbuf = newBuffer();
	E.buf = E.headbuf;

	/* Load piped stdin data if present */
	if (stdin_data != NULL) {
		if (stdin_len > 0) {
			/* Hard limit: no prompt for stdin */
			if (stdin_len > EMIL_MAX_FILE_SIZE) {
				free(stdin_data);
				disableRawMode();
				fprintf(stderr, "stdin: %s\n",
					"Exceeds 1 GiB limit");
				exit(1);
			}
			struct buffer *stdinBuf =
				loadStdinBuffer(stdin_data, stdin_len);
			if (stdinBuf == NULL) {
				/* Binary or invalid UTF-8: bail out
				 * cleanly */
				free(stdin_data);
				disableRawMode();
				fprintf(stderr, "stdin: %s\n",
					"Failed UTF-8 validation");
				exit(1);
			}
			stdinBuf->next = E.headbuf;
			E.headbuf = stdinBuf;
			E.buf = stdinBuf;
			stdin_buf_used = 1;
		}
		free(stdin_data);
		stdin_data = NULL;
	}

	if (argc >= 2) {
		int i = 1;
		int linum = -1;
		if (argv[1][0] == '+' && argc > 2) {
			linum = atoi(argv[1] + 1);
			i++;
		}
		for (; i < argc; i++) {
			/* POSIX: "-" means read from stdin */
			if (strcmp(argv[i], "-") == 0) {
				if (stdin_buf_used) {
					/* Already loaded stdin above */
					continue;
				}
				/* stdin was a tty and not piped:
				 * nothing to read */
				setStatusMessage("stdin: no piped input");
				continue;
			}

			struct buffer *newBuf = newBuffer();
			if (editorOpen(newBuf, argv[i]) < 0) {
				disableRawMode();

				fprintf(stderr, "%s: %s\n", argv[i],
					E.statusmsg);
				exit(1);
			}

			newBuf->next = E.headbuf;
			if (linum > 0) {
				if (linum - 1 >= newBuf->numrows) {
					newBuf->cy = newBuf->numrows - 1;
				} else {
					newBuf->cy = linum - 1;
				}
				linum = -1;
			}
			E.headbuf = newBuf;
			E.buf = newBuf;
		}
	}
	E.windows[0]->buf = E.buf;

	/* Initialize minibuffer */
	E.minibuf = newBuffer();
	E.minibuf->word_wrap = 0;
	E.minibuf->filename = xstrdup("*minibuffer*");
	E.minibuf->special_buffer = 1;
	E.edbuf = E.buf;
	computeDisplayNames();

	for (;;) {
		/* Also called from readKey(); repeated here so a flag
		 * raised before the first read (or between drained
		 * keys) is acted on without waiting for a keypress. */
		handlePendingSignals();
		refreshScreen();

		int key = readKey();
		if (key == -1)
			continue; /* signal interrupted: recheck flags */

		for (;;) {
			recordKey(key);

			if (key >= ' ' && key < KEY_ARROW_LEFT)
				E.self_insert_key = key;

			if (key != 033)
				E.statusmsg_show = 0;

			int cmd = resolveBinding(key);
			if (cmd != CMD_NONE)
				processKeypress(cmd);

			/* Check for more keys already in the buffer */
			fd_set fds;
			struct timeval tv = { 0, 0 };
			FD_ZERO(&fds);
			FD_SET(STDIN_FILENO, &fds);
			if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <=
			    0)
				break;

			/* Bytes were already waiting: this key and the
			 * next arrived together rather than being typed
			 * one at a time.  That is the only signal emil
			 * has that it is being pasted into (§3.5). */
			E.input_burst = 1;

			key = readKey();
			if (key == -1)
				break;
		}

		/* Burst over: the next key, whenever it comes, starts a
		 * fresh undo run rather than extending this one. */
		if (E.input_burst) {
			E.input_burst = 0;
			undoCloseRun(E.buf);
		}
	}
}
