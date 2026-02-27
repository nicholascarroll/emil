#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include "emil.h"
#include "message.h"
#include "fileio.h"
#include "find.h"
#include "pipe.h"
#include "region.h"
#include "register.h"
#include "history.h"
#include "buffer.h"
#include "completion.h"
#include "transform.h"
#include "undo.h"
#include "unicode.h"
#include "unused.h"
#include "terminal.h"
#include "display.h"
#include "keymap.h"
#include "util.h"

const int page_overlap = 2;

struct editorConfig E;
void setupHandlers(void);

/*** output ***/

void editorSuspend(int UNUSED(sig)) {
	signal(SIGTSTP, SIG_DFL);
	disableRawMode();
	raise(SIGTSTP);
}

void editorResume(int UNUSED(sig)) {
	/* Reset scrolling region in case we came back from a shell drawer */
	write(STDOUT_FILENO, CSI "r", 3);
	/* Restore cursor (matches ESC 7 in editorOpenShellDrawer) */
	write(STDOUT_FILENO, ESC "8", 2);
	setupHandlers();
	enableRawMode();

	/* Force all windows to recalculate heights for the restored screen */
	for (int i = 0; i < E.nwindows; i++)
		E.windows[i]->height = 0;

	editorResizeScreen(0);
}

#ifdef SIGWINCH
void sigwinchHandler(int UNUSED(sig)) {
	editorResizeScreen(0);
}
#endif

/*** init ***/

void setupHandlers(void) {
#ifdef SIGWINCH
	signal(SIGWINCH, sigwinchHandler);
#endif
	signal(SIGCONT, editorResume);
	signal(SIGTSTP, editorSuspend);
}

void initEditor(void) {
	E.statusmsg[0] = 0;
	E.kill = NULL;
	E.rectKill = NULL;
	E.windows = xmalloc(sizeof(struct editorWindow *) * 1);
	E.windows[0] = xcalloc(1, sizeof(struct editorWindow));
	E.windows[0]->focused = 1;
	E.nwindows = 1;
	E.recording = 0;
	E.macro.nkeys = 0;
	E.macro.keys = NULL;
	E.micro = 0;
	E.playback = 0;
	E.headbuf = NULL;
	memset(E.registers, 0, sizeof(E.registers));
	setupCommands(&E);
	E.lastVisitedBuffer = NULL;
	E.macro_depth = 0;

	initHistory(&E.file_history);
	initHistory(&E.command_history);
	initHistory(&E.shell_history);
	initHistory(&E.search_history);
	initHistory(&E.kill_history);
	E.kill_ring_pos = -1;

	if (getWindowSize(&E.screenrows, &E.screencols) == -1)
		die("getWindowSize");
}

/*
 * Read all available data from a file descriptor into a malloc'd buffer.
 * Sets *out_len to the number of bytes read.  Returns NULL on allocation
 * failure; returns an empty buffer (out_len == 0) if nothing was read.
 */
static char *readAllFromFd(int fd, size_t *out_len) {
	size_t cap = BUFSIZ;
	size_t len = 0;
	char *buf = xmalloc(cap);
	ssize_t n;
	while ((n = read(fd, buf + len, cap - len)) > 0) {
		len += (size_t)n;
		if (len >= cap) {
			cap <<= 1;
			buf = xrealloc(buf, cap);
		}
	}
	*out_len = len;
	return buf;
}

/*
 * Load piped stdin data into a new editor buffer.  The data is split
 * on newline boundaries and inserted row by row, matching the same
 * approach used by editorOpen().  The buffer is named "*stdin*" and
 * marked dirty so the user is prompted before discarding.
 *
 * Returns the new buffer, or NULL if the data contains null bytes
 * (which would indicate binary / non-UTF-8 content).
 */
static struct editorBuffer *loadStdinBuffer(const char *data, size_t len) {
	/* Reject binary data: null bytes can't be represented */
	if (memchr(data, '\0', len) != NULL) {
		return NULL;
	}

	struct editorBuffer *buf = newBuffer();
	buf->filename = xstrdup("*stdin*");

	size_t start = 0;
	for (size_t i = 0; i < len; i++) {
		if (data[i] == '\n') {
			/* Strip trailing \r for DOS line endings */
			size_t end = i;
			if (end > start && data[end - 1] == '\r')
				end--;
			editorInsertRow(buf, buf->numrows, (char *)&data[start],
					(int)(end - start));
			start = i + 1;
		}
	}
	/* Handle final line without trailing newline */
	if (start < len) {
		size_t end = len;
		if (end > start && data[end - 1] == '\r')
			end--;
		editorInsertRow(buf, buf->numrows, (char *)&data[start],
				(int)(end - start));
	}

	buf->dirty = 0;
	buf->read_only = 1;
	buf->word_wrap = 1;
	return buf;
}

int main(int argc, char *argv[]) {
	// Check for --version flag before entering raw mode
	if (argc >= 2 && strcmp(argv[1], "--version") == 0) {
		printf("emil %s\n", EMIL_VERSION);
		return 0;
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

	E.headbuf = newBuffer();
	E.buf = E.headbuf;

	/* Load piped stdin data if present */
	if (stdin_data != NULL) {
		if (stdin_len > 0) {
			struct editorBuffer *stdinBuf =
				loadStdinBuffer(stdin_data, stdin_len);
			if (stdinBuf == NULL) {
				/* Binary data — bail out cleanly */
				free(stdin_data);
				disableRawMode();
				fprintf(stderr,
					"stdin: data failed UTF-8 validation\n");
				exit(1);
			}
			stdinBuf->next = E.headbuf;
			E.headbuf = stdinBuf;
			E.buf = stdinBuf;
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
			struct editorBuffer *newBuf = newBuffer();
			if (editorOpen(newBuf, argv[i]) < 0) {
				disableRawMode();
				fprintf(stderr,
					"%s: file failed UTF-8 validation\n",
					argv[i]);
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
	E.edbuf = E.buf;
	computeDisplayNames();

#ifdef EMIL_DISABLE_SHELL
	editorSetStatusMessage("Shell integration disabled");
#endif /* EMIL_DISABLE_SHELL */
	setupHandlers();

	for (;;) {
		refreshScreen();

		int c = editorReadKey();
		if (c == MACRO_RECORD) {
			if (E.recording) {
				editorSetStatusMessage(
					"Already defining keyboard macro");
			} else {
				editorSetStatusMessage(
					"Defining keyboard macro...");
				E.recording = 1;
				E.macro.nkeys = 0;
				E.macro.skeys = 0x10;
				free(E.macro.keys);
				E.macro.keys =
					xmalloc(E.macro.skeys * sizeof(int));
			}
		} else if (c == MACRO_END) {
			if (E.recording) {
				editorSetStatusMessage(
					"Keyboard macro defined");
				E.recording = 0;
			} else {
				editorSetStatusMessage(
					"Not defining keyboard macro");
			}
		} else if (c == MACRO_EXEC ||
			   (E.micro == MACRO_EXEC && (c == 'e' || c == 'E'))) {
			if (E.recording) {
				editorSetStatusMessage(
					"Keyboard macro defined");
				E.recording = 0;
			}
			editorExecMacro(&E.macro);
			E.micro = MACRO_EXEC;
		} else {
			editorRecordKey(c);
			executeCommand(c);
		}
	}

	/* cleanup */
	free(E.kill);

	return 0;
}
