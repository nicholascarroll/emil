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
#include "message.h"
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

void editorSuspend(int sig) {
	(void)sig;
	IGNORE_RETURN(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios));
	IGNORE_RETURN(write(STDOUT_FILENO, CSI "?1049l", 8));
	signal(SIGTSTP, SIG_DFL);
	raise(SIGTSTP);
}

void editorResume(int sig) {
	(void)sig;
	got_sigcont = 1;
}

#ifdef SIGWINCH
void sigwinchHandler(int sig) {
	(void)sig;
	got_sigwinch = 1;
}
#endif

void handleSigterm(int sig) {
	(void)sig;
	got_sigterm = 1;
}

void handleSighup(int sig) {
	(void)sig;
	got_sighup = 1;
}

/*** init ***/

void setupHandlers(void) {
#ifdef SIGWINCH
	/* No SA_RESTART: the blocking read in readKey must return
	 * EINTR so the main loop sees got_sigwinch and repaints
	 * immediately.  With SA_RESTART the read was restarted and a
	 * resize produced no repaint until the next keypress.  All
	 * blocking reads/waits elsewhere already retry on EINTR
	 * (fileio, subprocess waitpid), matching how SIGCONT has
	 * always been installed. */
	install_handler(SIGWINCH, sigwinchHandler, 0);
#endif
	install_handler(SIGCONT, editorResume, 0);
	install_handler(SIGTSTP, editorSuspend, SA_NODEFER);
	install_handler(SIGTERM, handleSigterm, 0);
	install_handler(SIGHUP, handleSighup, 0);
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

void initEditor(void) {
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
	E.macro_depth = 0;

	initHistory(&E.file_history);
	initHistory(&E.command_history);
	initHistory(&E.shell_history);
	initHistory(&E.search_history);
	initHistory(&E.kill_history);
	E.kill_ring_pos = -1;

	E.render_buf = (struct abuf){ NULL, 0, 0 };

	if (getWindowSize(&E.screenrows, &E.screencols) == -1)
		die("getWindowSize");
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
		} else {
			printf("Unknown option argument %s\n", argv[1]);
			return 0;
		}
	}

	/*
	 * Detect piped stdin: if stdin is not a terminal, slurp the
	 * data before entering raw mode, then reopen /dev/tty so the
	 * terminal works normally.  This enables:
	 *   git diff | emil
	 *   curl ... | emil
	 *   grep -rn foo | emil
	 */
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
					msg_memory_limit);
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
					msg_invalid_utf8);
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
				setStatusMessage(msg_no_piped_input);
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
				if (newBuf->numrows == 0) {
					newBuf->cy = 0;
				} else if (linum - 1 >= newBuf->numrows) {
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
	E.minibuf->single_line = 1;
	E.minibuf->word_wrap = 0;
	E.minibuf->filename = xstrdup("*minibuffer*");
	E.minibuf->special_buffer = 1;
	E.edbuf = E.buf;
	computeDisplayNames();

#ifdef EMIL_DISABLE_SHELL
	if (!E.statusmsg_show)
		setStatusMessage(msg_shell_disabled);
#endif /* EMIL_DISABLE_SHELL */
	for (;;) {
		if (got_sigterm || got_sighup) {
			disableRawMode();
			_exit(1);
		}
		if (got_sigwinch) {
			got_sigwinch = 0;
			resizeScreen(0);
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
			resizeScreen(0);
			resetFileCheckThrottle();
		}
		refreshScreen();

		int key = readKey();
		if (key == -1)
			continue; /* signal interrupted: recheck flags */

		/*
		 * Process this key and then drain any additional
		 * keys already queued in the input buffer before
		 * repainting.  This avoids a full refreshScreen()
		 * between every single keystroke, which fixes two
		 * classes of bugs:
		 *
		 *  1. Perceived command reordering (#51): rapid
		 *     C-SPC C-a appears to move-then-mark because
		 *     the expensive refresh between the two keys
		 *     delays the visual feedback.
		 *
		 *  2. Modifier key desync (#52): the large write()
		 *     in refreshScreen() floods the terminal
		 *     emulator with output, creating a window where
		 *     modifier key state (Ctrl) can fall out of sync
		 *     with the next keypress, causing literal a/e to
		 *     be inserted instead of C-a/C-e.
		 *
		 * The first key is read with a blocking read();
		 * subsequent keys are drained with a non-blocking
		 * select() check: we only consume what is already
		 * buffered, never wait for more.
		 */
		for (;;) {
			recordKey(key);

			if (key >= ' ' && key < KEY_ARROW_LEFT)
				E.self_insert_key = key;

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

			key = readKey();
			if (key == -1)
				break;
		}
	}
}
