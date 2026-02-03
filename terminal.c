#include "util.h"
#include "terminal.h"
#include "emil.h"
#include "message.h"
#include "base64.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#ifdef __sun
#include <sys/types.h> /* This might be needed first */
#include <sys/termios.h>
#endif
#include <sys/ioctl.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include "unicode.h"
#include "keymap.h"
#include "display.h"

extern struct editorConfig E;
void editorDeserializeUnicode(void);

void die(const char *s) {
	write(STDOUT_FILENO, CSI "2J", 4);
	write(STDOUT_FILENO, CSI "H", 3);
	perror(s);
	write(STDOUT_FILENO, CRLF, 2);
	exit(1);
}

void disableRawMode(void) {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("disableRawMode tcsetattr");
	if (write(STDOUT_FILENO, CSI "?1049l", 8) == -1)
		die("disableRawMode write");
}

/*
 * Restore cooked terminal mode without leaving the alternate screen
 * buffer.  Used by the shell drawer so that the editor content painted
 * in the upper portion of the alt screen stays visible while the shell
 * runs in the bottom portion.
 */
void disableRawModeKeepScreen(void) {
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios) == -1)
		die("disableRawModeKeepScreen tcsetattr");
}

/*
 * Shell drawer — opens a small shell region at the bottom of the
 * terminal while the editor content above stays frozen.
 *
 * Mechanism:
 *   1. Set the DECSTBM scrolling region to the bottom N rows.
 *   2. Move the cursor into the drawer area and print a header.
 *   3. Restore cooked mode (without leaving the alt screen).
 *   4. raise(SIGTSTP) — the parent shell prints its prompt inside the
 *      restricted scrolling region; everything above is protected.
 *   5. On SIGCONT (user typed `fg`), the handler resets the scrolling
 *      region, re-enters raw mode, and redraws — closing the drawer.
 */


void editorOpenShellDrawer(void) {
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_row < 12)
		return;

	int drawerHeight = ws.ws_row / 3;
	if (drawerHeight < 6)
		drawerHeight = 6;

	/*
	 * Shrink the editor to fit in the top portion of the screen.
	 * The bottom window's modeline becomes the visual separator
	 * between the editor and the shell drawer area.
	 */
	int editorRows = ws.ws_row - drawerHeight;
	E.screenrows = editorRows;

	/* Force all windows to recalculate heights for the smaller space */
	for (int i = 0; i < E.nwindows; i++)
		E.windows[i]->height = 0;

	/* Save cursor position */
	if (write(STDOUT_FILENO, ESC "7", 2) != 2)
		return;

	/* Repaint the editor into the smaller area */
	refreshScreen();

	/*
	 * The editor content now occupies rows 1..editorRows.
	 * The minibuffer sits at row editorRows; start the drawer
	 * there so the clear below erases it, leaving the bottom
	 * window's modeline as the boundary.
	 */
	int drawerTop = editorRows;
	char buf[64];
	int n = snprintf(buf, sizeof(buf), CSI "%d;%dr", drawerTop, ws.ws_row);
	if (n > 0)
		write(STDOUT_FILENO, buf, n);

	/* Move cursor to the top of the drawer and clear the area */
	n = snprintf(buf, sizeof(buf), CSI "%d;1H", drawerTop);
	if (n > 0)
		write(STDOUT_FILENO, buf, n);
	write(STDOUT_FILENO, CSI "J", 3);

	/* Restore cooked mode but stay on the alt screen */
	disableRawModeKeepScreen();

	/* Let the shell take over */
	signal(SIGTSTP, SIG_DFL);
	raise(SIGTSTP);
}


void enableRawMode(void) {
	/* Saves the screen and switches to an alt screen */
	if (write(STDOUT_FILENO, CSI "?1049h", 8) == -1)
		die("enableRawMode write");

	/*
	 * I looked into it. It's possible, but not easy, to do it
	 * without termios. Basically you'd have to hand-hack and send
	 * off your own bits. Check out busybox vi and that rabbithole
	 * for an implementation.
	 */
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");
	atexit(disableRawMode);

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("enableRawMode tcsetattr");
}

int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;

	while (i < (int)(sizeof(buf) - 1)) {
		if (read(STDIN_FILENO, &buf[i], 1) != 1)
			break;
		if (buf[i] == 'R')
			break;
		i++;
	}
	buf[i] = '\0';

	if (buf[0] != '\x1b' || buf[1] != '[')
		return -1;
	if (sscanf(&buf[2], "%d;%d", rows, cols) != 2)
		return -1;

	return 0;
}

int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
		if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
			return -1;
		return getCursorPosition(rows, cols);
	} else {
		*cols = ws.ws_col;
		*rows = ws.ws_row;
		return 0;
	}
}

void editorCopyToClipboard(const uint8_t *text) {
	if (text == NULL || text[0] == '\0')
		return;

	char *encoded = base64_encode(text, strlen((const char *)text));
	if (encoded == NULL)
		return;

	/*
	 * OSC 52: \033]52;c;<base64>\033\\
	 *
	 * "c" targets the system clipboard. The ST (String Terminator)
	 * is \033\\ (ESC backslash), which is more portable than BEL
	 * across terminal emulators and tmux.
	 */
	write(STDOUT_FILENO, "\033]52;c;", 7);
	write(STDOUT_FILENO, encoded, strlen(encoded));
	write(STDOUT_FILENO, "\033\\", 2);

	free(encoded);
}

void editorDeserializeUnicode(void) {
	E.unicode[0] = E.macro.keys[E.playback++];
	E.nunicode = utf8_nBytes(E.unicode[0]);
	for (int i = 1; i < E.nunicode; i++) {
		E.unicode[i] = E.macro.keys[E.playback++];
	}
}

/* Raw reading a keypress - terminal layer only handles raw byte reading and escape sequences */
int editorReadKey(void) {
	if (E.playback) {
		int ret = E.macro.keys[E.playback++];
		if (ret == UNICODE) {
			editorDeserializeUnicode();
		}
		return ret;
	}
	int nread;
	uint8_t c;
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}
	if (c == CTRL('u')) {
		return UNIVERSAL_ARGUMENT;
	}
	if (c == 033) {
		char seq[5] = { 0, 0, 0, 0, 0 };
		if (read(STDIN_FILENO, &seq[0], 1) != 1)
			goto ESC_UNKNOWN;

		if (seq[0] == '[') {
			if (read(STDIN_FILENO, &seq[1], 1) != 1)
				goto ESC_UNKNOWN;
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					goto ESC_UNKNOWN;
				if (seq[2] == '~') {
					switch (seq[1]) {
					case '1':
						return HOME_KEY;
					case '3':
						return DEL_KEY;
					case '4':
						return END_KEY;
					case '5':
						return PAGE_UP;
					case '6':
						return PAGE_DOWN;
					case '7':
						return HOME_KEY;
					case '8':
						return END_KEY;
					}
				} else if (seq[2] == '4') {
					if (read(STDIN_FILENO, &seq[3], 1) != 1)
						goto ESC_UNKNOWN;
					if (seq[3] == '~') {
						errno = EINTR;
						die("Panic key");
					}
				}
			} else {
				switch (seq[1]) {
				case 'A':
					return ARROW_UP;
				case 'B':
					return ARROW_DOWN;
				case 'C':
					return ARROW_RIGHT;
				case 'D':
					return ARROW_LEFT;
				case 'F':
					return END_KEY;
				case 'H':
					return HOME_KEY;
				case 'Z':
					return BACKTAB;
				}
			}
		} else if ('0' <= seq[0] && seq[0] <= '9') {
			return ALT_0 + (seq[0] - '0');
		} else if (seq[0] == '<') {
			return BEG_OF_FILE;
		} else if (seq[0] == '>') {
			return END_OF_FILE;
		} else if (seq[0] == '|') {
			return PIPE_CMD;
		} else if (seq[0] == '%') {
			return QUERY_REPLACE;
		} else if (seq[0] == '?') {
			return CUSTOM_INFO_MESSAGE;
		} else if (seq[0] == '/') {
			return EXPAND;
		} else if (seq[0] == 127) {
			return BACKSPACE_WORD;
		} else if (seq[0] == CTRL('s')) {
			return REGEX_SEARCH_FORWARD;
		} else if (seq[0] == CTRL('r')) {
			return REGEX_SEARCH_BACKWARD;
		} else if (seq[0] == 'p') {
			return HISTORY_PREV;
		} else if (seq[0] == 'n') {
			return HISTORY_NEXT;
		} else {
			switch ((seq[0] & 0x1f) | 0x40) {
			case 'B':
				return BACKWARD_WORD;
			case 'C':
				return CAPCASE_WORD;
			case 'D':
				return DELETE_WORD;
			case 'F':
				return FORWARD_WORD;
			case 'G':
				return GOTO_LINE;
			case 'H':
				return BACKSPACE_WORD;
			case 'L':
				return DOWNCASE_WORD;
			case 'N':
				return FORWARD_PARA;
			case 'P':
				return BACKWARD_PARA;
			case 'T':
				return TRANSPOSE_WORDS;
			case 'U':
				return UPCASE_WORD;
			case 'V':
				return PAGE_UP;
			case 'W':
				return COPY;
			case 'X':
				return EXEC_CMD;
			case 'Y':
				return YANK_POP;
			}
		}

ESC_UNKNOWN:;
		char seqR[32];
		seqR[0] = 0;
		char buf[8];
		for (int i = 0; seq[i]; i++) {
			if (seq[i] < ' ') {
				snprintf(buf, sizeof(buf), "C-%c ",
					 seq[i] + '`');
			} else {
				snprintf(buf, sizeof(buf), "%c ", seq[i]);
			}
			emil_strlcat(seqR, buf, sizeof(seqR));
		}
		editorSetStatusMessage("Unknown command M-%s", seqR);
		return 033;
	} else if (utf8_is2Char(c)) {
		/* 2-byte UTF-8 sequence */
		E.nunicode = 2;

		E.unicode[0] = c;
		if (read(STDIN_FILENO, &E.unicode[1], 1) != 1)
			return UNICODE_ERROR;
		return UNICODE;
	} else if (utf8_is3Char(c)) {
		/* 3-byte UTF-8 sequence */
		E.nunicode = 3;

		E.unicode[0] = c;
		if (read(STDIN_FILENO, &E.unicode[1], 1) != 1)
			return UNICODE_ERROR;
		if (read(STDIN_FILENO, &E.unicode[2], 1) != 1)
			return UNICODE_ERROR;
		return UNICODE;
	} else if (utf8_is4Char(c)) {
		/* 4-byte UTF-8 sequence */
		E.nunicode = 4;

		E.unicode[0] = c;
		if (read(STDIN_FILENO, &E.unicode[1], 1) != 1)
			return UNICODE_ERROR;
		if (read(STDIN_FILENO, &E.unicode[2], 1) != 1)
			return UNICODE_ERROR;
		if (read(STDIN_FILENO, &E.unicode[3], 1) != 1)
			return UNICODE_ERROR;
		return UNICODE;
	}
	return c;
}
