#include "util.h"
#include "terminal.h"
#include "emil.h"
#include "message.h"
#include "base64.h"
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <time.h>
#ifdef __sun
#include <sys/types.h> /* This might be needed first */
#include <sys/termios.h>
#endif
#include <sys/ioctl.h>
#include <sys/select.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include "unicode.h"
#include "keymap.h"
#include "display.h"

extern struct config E;
void deserializeUnicode(void);

void install_handler(int signum, void (*handler)(int), int flags) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = flags;
	sigaction(signum, &sa, NULL);
}

void die(const char *s) {
	/* die() ends in exit(), which runs the atexit handlers.  If
	 * anything on that path (or editorCleanup below) routes back
	 * here, calling exit() a second time: from within exit
	 * processing: is undefined behavior.  Detect re-entry and
	 * leave immediately without re-running any cleanup. */
	static int dying = 0;
	if (dying)
		_exit(1);
	dying = 1;
	IGNORE_RETURN(write(STDOUT_FILENO, CSI "2J", 4));
	IGNORE_RETURN(write(STDOUT_FILENO, CSI "H", 3));
	perror(s);
	IGNORE_RETURN(write(STDOUT_FILENO, CRLF, 2));
	editorCleanup();
	exit(1);
}

/* Registered with atexit(); also called directly before printing
 * fatal errors.  Best-effort by design: this runs during exit
 * processing, where routing a failure through die() would call
 * exit() inside exit() (undefined behavior).  If tcsetattr fails
 * here the terminal is beyond saving anyway. */
void disableRawMode(void) {
	IGNORE_RETURN(tcsetattr(STDIN_FILENO, TCSAFLUSH, &E.orig_termios));
	IGNORE_RETURN(write(STDOUT_FILENO, CSI "?1049l", 8));
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
 * Shell drawer: opens a small shell region at the bottom of the
 * terminal while the editor content above stays frozen.
 *
 * Mechanism:
 *   1. Set the DECSTBM scrolling region to the bottom N rows.
 *   2. Move the cursor into the drawer area and print a header.
 *   3. Restore cooked mode (without leaving the alt screen).
 *   4. raise(SIGTSTP): the parent shell prints its prompt inside the
 *      restricted scrolling region; everything above is protected.
 *   5. On SIGCONT (user typed `fg`), the handler resets the scrolling
 *      region, re-enters raw mode, and redraws: closing the drawer.
 */

void openShellDrawer(void) {
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
	int rows = ws.ws_row - drawerHeight;
	E.screenrows = rows;

	/* Force all windows to recalculate heights for the smaller space */
	for (int i = 0; i < E.nwindows; i++)
		E.windows[i]->height = 0;

	/* Save cursor position */
	if (write(STDOUT_FILENO, ESC "7", 2) != 2)
		return;

	/* Repaint the editor into the smaller area */
	refreshScreen();

	/*
	 * The editor content now occupies rows 1..rows.
	 * The minibuffer sits at row rows; start the drawer
	 * there so the clear below erases it, leaving the bottom
	 * window's modeline as the boundary.
	 */
	int drawerTop = rows;
	char buf[64];
	int n = snprintf(buf, sizeof(buf), CSI "%d;%dr", drawerTop, ws.ws_row);
	if (n > 0)
		IGNORE_RETURN(write(STDOUT_FILENO, buf, n));

	/* Move cursor to the top of the drawer and clear the area */
	n = snprintf(buf, sizeof(buf), CSI "%d;1H", drawerTop);
	if (n > 0)
		IGNORE_RETURN(write(STDOUT_FILENO, buf, n));
	IGNORE_RETURN(write(STDOUT_FILENO, CSI "J", 3));

	/* Restore cooked mode but stay on the alt screen */
	disableRawModeKeepScreen();

	/* Let the shell take over */
	signal(SIGTSTP, SIG_DFL);
	raise(SIGTSTP);
}

/*
 * Apply raw-mode terminal settings without saving orig_termios.
 * Used on resume (SIGCONT) where the original state is already saved.
 */
void applyRawMode(void) {
	/* Switch to alternate screen */
	if (write(STDOUT_FILENO, CSI "?1049h", 8) == -1)
		die("applyRawMode write");

	struct termios raw = E.orig_termios;
	raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
	raw.c_oflag &= ~(OPOST);
	raw.c_cflag |= (CS8);
	raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
	raw.c_cc[VMIN] = 1;
	raw.c_cc[VTIME] = 0;
	if (tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw) == -1)
		die("applyRawMode tcsetattr");
}

void enableRawMode(void) {
	if (tcgetattr(STDIN_FILENO, &E.orig_termios) == -1)
		die("tcgetattr");
	atexit(disableRawMode);

	applyRawMode();
}

/*
 * Milliseconds to wait for the terminal's CSI 6n reply.
 *
 * This path only runs when TIOCGWINSZ told us nothing, which in
 * practice means a serial console: the RS-232 line carries no
 * out-of-band window size, so the kernel has none to report and we
 * have to ask the terminal itself.
 */
#define CPR_TIMEOUT_MS 500

/*
 * After giving up, how long to keep discarding a reply that shows up
 * late (total budget, and how long to wait for each further byte).
 */
#define CPR_DRAIN_MS 500
#define CPR_DRAIN_IDLE_MS 200

int getCursorPosition(int *rows, int *cols) {
	char buf[32];
	int i = 0;

	if (write(STDOUT_FILENO, "\x1b[6n", 4) != 4)
		return -1;

	/* One deadline for the whole reply, not per byte: a far end
	 * that dribbles a byte just under the limit forever must not be
	 * able to keep us here. */
	struct timespec start;
	if (clock_gettime(CLOCK_MONOTONIC, &start) == -1)
		return -1;

	while (i < (int)(sizeof(buf) - 1)) {
		struct timespec now;
		if (clock_gettime(CLOCK_MONOTONIC, &now) == -1)
			return -1;
		long elapsed = (now.tv_sec - start.tv_sec) * 1000 +
			       (now.tv_nsec - start.tv_nsec) / 1000000;
		int remaining = (int)(CPR_TIMEOUT_MS - elapsed);
		if (remaining <= 0)
			return -1;

		struct pollfd pfd;
		pfd.fd = STDIN_FILENO;
		pfd.events = POLLIN;
		pfd.revents = 0;

		int pr = poll(&pfd, 1, remaining);
		if (pr == -1) {
			if (errno == EINTR)
				continue; /* SIGWINCH etc; deadline still holds */
			return -1;
		}
		if (pr == 0) {
			/* Gave up waiting. */
			char drain;
			struct pollfd d;
			struct timespec dstart, dnow;
			d.fd = STDIN_FILENO;
			d.events = POLLIN;
			d.revents = 0;
			if (clock_gettime(CLOCK_MONOTONIC, &dstart) == -1)
				return -1;
			for (;;) {
				if (clock_gettime(CLOCK_MONOTONIC, &dnow) == -1)
					break;
				long spent =
					(dnow.tv_sec - dstart.tv_sec) * 1000 +
					(dnow.tv_nsec - dstart.tv_nsec) /
						1000000;
				if (spent >= CPR_DRAIN_MS)
					break;
				int dp = poll(&d, 1, CPR_DRAIN_IDLE_MS);
				if (dp == -1 && errno == EINTR)
					continue; /* budget still bounds us */
				if (dp != 1)
					break;
				ssize_t dn = read(STDIN_FILENO, &drain, 1);
				if (dn == -1 && errno == EINTR)
					continue;
				if (dn != 1)
					break;
			}
			return -1;
		}

		ssize_t rn = read(STDIN_FILENO, &buf[i], 1);
		if (rn == -1 && errno == EINTR)
			continue; /* signal between poll and read; deadline
				   * still holds, go around again */
		if (rn != 1)
			return -1; /* 0 is EOF: the line dropped */
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

static int window_size_was_probed = 0;

int windowSizeWasProbed(void) {
	return window_size_was_probed;
}

/*
 * Ask the terminal itself for its size: park the cursor at the
 * bottom-right corner and read it back with CPR.  On success the
 * kernel's idea of the size is updated too, so child processes
 * (shell integration) inherit the right dimensions.  On failure the
 * caller's rows/cols are left untouched, so a failed re-probe keeps
 * the current geometry rather than resetting it.
 */
int probeWindowSize(int *rows, int *cols) {
	int r, c;

	if (write(STDOUT_FILENO, "\x1b[999C\x1b[999B", 12) != 12)
		return -1;
	if (getCursorPosition(&r, &c) != 0)
		return -1;

	struct winsize ws;
	ws.ws_row = (unsigned short)r;
	ws.ws_col = (unsigned short)c;
	ws.ws_xpixel = 0;
	ws.ws_ypixel = 0;
	IGNORE_RETURN(ioctl(STDOUT_FILENO, TIOCSWINSZ, &ws));

	*rows = r;
	*cols = c;
	return 0;
}

/*
 * Always succeeds (returns 0): every failure path inside falls back
 * to a usable size rather than reporting an error.
 */
int getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0 ||
	    ws.ws_row == 0) {
		window_size_was_probed = 1;
		if (probeWindowSize(rows, cols) != 0) {
			*rows = 24;
			*cols = 80;
		}
		return 0;
	}

	*cols = ws.ws_col;
	*rows = ws.ws_row;
	return 0;
}

void copyToClipboard(const uint8_t *text) {
	if (text == NULL || text[0] == '\0')
		return;

	/*
	 * OSC 52 sets the system clipboard: \033]52;c;<base64>\033\\
	 * Tmux intercepts and forwards this without DCS wrapping.
	 * xterm caps the sequence at 100000 bytes (74993 raw).
	 */

	size_t textlen = strlen((const char *)text);
	if (textlen > 74993) {
		setStatusMessage(msg_osc52_too_large, (int)textlen, 74993);
		return;
	}

	char *encoded = base64_encode(text, textlen);
	if (encoded == NULL)
		return;

	size_t elen = strlen(encoded);
	size_t total = 7 + elen + 2; /* \033]52;c; + payload + \033\\ */
	char *buf = xmalloc(total);
	if (buf == NULL) {
		free(encoded);
		return;
	}

	memcpy(buf, "\033]52;c;", 7);
	memcpy(buf + 7, encoded, elen);
	memcpy(buf + 7 + elen, "\033\\", 2);

	IGNORE_RETURN(write(STDOUT_FILENO, buf, total));

	free(buf);
	free(encoded);
}

void deserializeUnicode(void) {
	/* Guard every read: a truncated macro must not index past
	 * nkeys into uninitialized key slots.  On truncation fall
	 * back to a replacement character so callers still see a
	 * valid (if wrong) UTF-8 sequence. */
	if (E.playback >= E.macro.nkeys) {
		E.unicode[0] = '?';
		E.nunicode = 1;
		return;
	}
	E.unicode[0] = E.macro.keys[E.playback++];
	E.nunicode = utf8_nBytes(E.unicode[0]);
	for (int i = 1; i < E.nunicode; i++) {
		if (E.playback >= E.macro.nkeys) {
			E.unicode[0] = '?';
			E.nunicode = 1;
			return;
		}
		E.unicode[i] = E.macro.keys[E.playback++];
	}
}

/*
 * Drain the remainder of an unrecognized CSI (ESC [) sequence.
 *
 * CSI sequences follow the ECMA-48 grammar:
 *   CSI  P..P  I..I  F
 * where P (parameter bytes) are 0x30-0x3F, I (intermediate bytes) are
 * 0x20-0x2F, and F (final byte) is 0x40-0x7E.  We read and discard
 * bytes until we consume the final byte or the input dries up.
 *
 * last_read is the most recent byte already consumed by the caller;
 * if it is itself a final byte there is nothing left to drain.
 */
static void drainCSI(uint8_t last_read) {
	if (last_read >= 0x40 && last_read <= 0x7E)
		return;

	for (;;) {
		fd_set fds;
		struct timeval tv = { 0, 50000 }; /* 50 ms */
		FD_ZERO(&fds);
		FD_SET(STDIN_FILENO, &fds);
		if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <= 0)
			break;
		uint8_t discard;
		if (read(STDIN_FILENO, &discard, 1) != 1)
			break;
		if (discard >= 0x40 && discard <= 0x7E)
			break;
	}
}

/* Single-byte input pushback.  The incremental-search scan probes
 * stdin between rows so C-g can cancel a long scan; a probed byte
 * that is NOT C-g is legitimate type-ahead and is returned here so
 * readKey delivers it in order.  One byte suffices: the probe never
 * reads past the first pending byte, so any escape-sequence
 * continuation bytes are still queued on the fd where readKey's
 * sequence reads expect them. */
static int input_pushback = -1;

void terminalPushbackByte(uint8_t c) {
	input_pushback = c;
}

int terminalPushbackPending(void) {
	return input_pushback >= 0;
}

/* Raw reading a keypress - terminal layer only handles raw byte
 * reading, escape sequence decoding, and UTF-8 assembly.
 * Returns key tokens only: no binding policy. */
int readKey(void) {
	if (E.playback) {
		/* A nested readKey (prefix sub-key, confirmation
		 * prompt) can be reached with the macro already
		 * exhausted if the recording missed a key.  Reading
		 * past nkeys returns uninitialized ints from the
		 * keys array; fail the read instead. */
		if (E.playback >= E.macro.nkeys)
			return -1;
		int ret = E.macro.keys[E.playback++];
		if (ret == KEY_UNICODE) {
			deserializeUnicode();
		}
		return ret;
	}
	int nread;
	uint8_t c;
	if (input_pushback >= 0) {
		/* Byte probed (and preserved) by the search-scan
		 * interrupt poll. */
		c = (uint8_t)input_pushback;
		input_pushback = -1;
	} else {
		while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
			if (nread == -1 && errno == EINTR)
				return -1;
			if (nread == -1 && errno != EAGAIN)
				die("read");
		}
	}
	if (c == 033) {
		char seq[5] = { 0, 0, 0, 0, 0 };
		if (read(STDIN_FILENO, &seq[0], 1) != 1)
			goto ESC_UNKNOWN;

		if (seq[0] == '[') {
			/* CSI sequence: physical key decoding */
			if (read(STDIN_FILENO, &seq[1], 1) != 1)
				goto ESC_UNKNOWN;
			if (seq[1] >= '0' && seq[1] <= '9') {
				if (read(STDIN_FILENO, &seq[2], 1) != 1)
					goto ESC_UNKNOWN;
				if (seq[2] == '~') {
					switch (seq[1]) {
					case '1':
						return KEY_HOME;
					case '3':
						return KEY_DEL;
					case '4':
						return KEY_END;
					case '5':
						return KEY_PAGE_UP;
					case '6':
						return KEY_PAGE_DOWN;
					case '7':
						return KEY_HOME;
					case '8':
						return KEY_END;
					}
				}
				/* Multi-digit CSI codes (\x1b[15~ .. \x1b[24~,
				 * i.e. F5-F12 and friends) fall through to
				 * ESC_UNKNOWN below, where drainCSI consumes
				 * the rest of the sequence harmlessly. A
				 * previous "panic key" branch here matched
				 * \x1b[<digit>4~ and called die(), which
				 * discarded unsaved buffers on F12 (\x1b[24~,
				 * xterm/rxvt) and F4 (\x1b[14~, PuTTY/vt220
				 * keyboards). */
			} else {
				switch (seq[1]) {
				case 'A':
					return KEY_ARROW_UP;
				case 'B':
					return KEY_ARROW_DOWN;
				case 'C':
					return KEY_ARROW_RIGHT;
				case 'D':
					return KEY_ARROW_LEFT;
				case 'F':
					return KEY_END;
				case 'H':
					return KEY_HOME;
				case 'Z':
					return KEY_BACKTAB;
				}
			}
		} else if (seq[0] == 'O') {
			/* SS3: xterm-family F1-F4 (\x1bOP..\x1bOS), and
			 * the cursor/Home/End keys when a previous
			 * program left the terminal in application
			 * cursor mode (\x1bOA-\x1bOD, \x1bOH, \x1bOF).
			 * Without this branch the 'O' returned as
			 * KEY_META('O') and the final byte leaked into
			 * the buffer as typed text.
			 *
			 * A short poll distinguishes a real SS3
			 * sequence (final byte already queued: terminals
			 * send sequences atomically) from Alt+Shift+O
			 * (nothing follows), which must keep returning
			 * KEY_META('O') without eating the next key. */
			fd_set fds;
			struct timeval tv = { 0, 50000 }; /* 50 ms */
			FD_ZERO(&fds);
			FD_SET(STDIN_FILENO, &fds);
			if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) <=
			    0)
				return KEY_META((uint8_t)seq[0]);
			if (read(STDIN_FILENO, &seq[1], 1) != 1)
				return KEY_META((uint8_t)seq[0]);
			switch (seq[1]) {
			case 'A':
				return KEY_ARROW_UP;
			case 'B':
				return KEY_ARROW_DOWN;
			case 'C':
				return KEY_ARROW_RIGHT;
			case 'D':
				return KEY_ARROW_LEFT;
			case 'H':
				return KEY_HOME;
			case 'F':
				return KEY_END;
			}
			/* F1-F4 and other SS3 finals: fall through to
			 * ESC_UNKNOWN, which reports the unrecognized
			 * key.  The final byte is already consumed, so
			 * the SS3 drain there must not read another. */
			goto ESC_UNKNOWN;
		} else if ('0' <= seq[0] && seq[0] <= '9') {
			/* Alt digit */
			return KEY_ALT_0 + (seq[0] - '0');
		} else {
			/* Meta + character: return as KEY_META(ch) */
			return KEY_META((uint8_t)seq[0]);
		}

ESC_UNKNOWN:
		/*
		 * Drain any remaining bytes that belong to this
		 * escape sequence so they are not misinterpreted
		 * as individual keypresses.
		 */
		if (seq[0] == '[') {
			uint8_t last = 0;
			for (int i = 4; i >= 1; i--) {
				if (seq[i]) {
					last = (uint8_t)seq[i];
					break;
				}
			}
			if (last)
				drainCSI(last);
		} else if (seq[0] == 'O' && seq[1] == 0) {
			/* SS3 reached with the final byte not yet
			 * consumed (read failure in the branch above):
			 * consume the single expected byte */
			fd_set fds;
			struct timeval tv = { 0, 50000 };
			FD_ZERO(&fds);
			FD_SET(STDIN_FILENO, &fds);
			if (select(STDIN_FILENO + 1, &fds, NULL, NULL, &tv) >
			    0) {
				uint8_t discard;
				if (read(STDIN_FILENO, &discard, 1) < 1) {
					/* nothing to do */
				}
			}
		}

		{
			char seqR[32];
			seqR[0] = 0;
			char buf[8];
			for (int i = 0; seq[i]; i++) {
				if (seq[i] < ' ') {
					snprintf(buf, sizeof(buf), "C-%c ",
						 seq[i] + '`');
				} else {
					snprintf(buf, sizeof(buf), "%c ",
						 seq[i]);
				}
				emil_strlcat(seqR, buf, sizeof(seqR));
			}
			setStatusMessage(msg_unknown_meta, seqR);
		}
		return 033;
	} else if (utf8_is2Char(c)) {
		E.nunicode = 2;
		E.unicode[0] = c;
		if (read(STDIN_FILENO, &E.unicode[1], 1) != 1)
			return KEY_UNICODE_ERROR;
		if (!utf8_validate(E.unicode, 2))
			return KEY_UNICODE_ERROR;
		return KEY_UNICODE;
	} else if (utf8_is3Char(c)) {
		E.nunicode = 3;
		E.unicode[0] = c;
		if (read(STDIN_FILENO, &E.unicode[1], 1) != 1)
			return KEY_UNICODE_ERROR;
		if (read(STDIN_FILENO, &E.unicode[2], 1) != 1)
			return KEY_UNICODE_ERROR;
		if (!utf8_validate(E.unicode, 3))
			return KEY_UNICODE_ERROR;
		return KEY_UNICODE;
	} else if (utf8_is4Char(c)) {
		E.nunicode = 4;
		E.unicode[0] = c;
		if (read(STDIN_FILENO, &E.unicode[1], 1) != 1)
			return KEY_UNICODE_ERROR;
		if (read(STDIN_FILENO, &E.unicode[2], 1) != 1)
			return KEY_UNICODE_ERROR;
		if (read(STDIN_FILENO, &E.unicode[3], 1) != 1)
			return KEY_UNICODE_ERROR;
		if (!utf8_validate(E.unicode, 4))
			return KEY_UNICODE_ERROR;
		return KEY_UNICODE;
	}
	return c;
}
