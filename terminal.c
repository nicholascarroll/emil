/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include "util.h"
#include "terminal.h"
#include "decoder.h"
#include "emil.h"

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

void installHandler(int signum, void (*handler)(int), int flags) {
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

	/* The drawer raises SIGTSTP itself with the handler reset, so
	 * editorSuspend() never runs.  Reaching this line means the stop
	 * was discarded (orphaned process group) and the editor is
	 * running on with cooked mode and a DECSTBM region still set:
	 * ask for the same repair a real `fg` gets. */
	requestTerminalResume();
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

void getWindowSize(int *rows, int *cols) {
	struct winsize ws;

	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0 &&
	    ws.ws_col > 0) {
		*rows = ws.ws_row;
		*cols = ws.ws_col;
	} else {
		*rows = 24;
		*cols = 80;
	}
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
		setStatusMessage(
			"Selection too large for OSC 52 clipboard (%d bytes, limit %d)",
			(int)textlen, 74993);
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
 * Escape-sequence input: the grammar and key mapping live in the
 * pure state machine in decoder.c; this file supplies only the byte
 * source (the clock and signal policy) and the reporting of
 * unrecognized sequences.
 */
/* Byte source for the decoder (see decoder.h for the contract).
 *
 * Both wait classes block.  They differ only in what a signal means.
 *
 * wait_indefinitely: the byte after a raw ESC.  ESC is the Meta
 * prefix, so block until the user continues.  A signal (EINTR)
 * abandons the wait so the main loop regains control to handle
 * resize/suspend flags; the pending ESC then decodes as a silent
 * bare ESC token.
 *
 * Otherwise: a byte inside a terminal-generated sequence.  Block,
 * retrying on EINTR, until it arrives.  */
/* Read one UTF-8 continuation byte, retrying on EINTR.
 *
 * Same rule as terminalEscByte: never abandon a sequence in flight.  A
 * bare read() here drops the whole character when a SIGWINCH or
 * SIGCONT arrives mid-sequence. */
static int terminalContByte(uint8_t *out) {
	for (;;) {
		ssize_t n = read(STDIN_FILENO, out, 1);
		if (n == 1)
			return 1;
		if (n == -1 && errno == EINTR)
			continue;
		return 0;
	}
}

static int terminalEscByte(uint8_t *out, int wait_indefinitely) {
	for (;;) {
		ssize_t n = read(STDIN_FILENO, out, 1);
		if (n == 1)
			return 1;
		if (n == -1 && errno == EINTR && !wait_indefinitely)
			continue; /* never abandon a sequence in flight */
		return 0;
	}
}

/* Report an unrecognized escape sequence in the status line, then
 * decode it as a bare ESC token (which the keymap ignores).  Control
 * bytes render as C-<letter>, matching the keymap's own notation. */
static int unknownEscape(const uint8_t *bytes, int n) {
	char seqR[32];
	char buf[8];
	seqR[0] = 0;
	for (int i = 0; i < n; i++) {
		if (bytes[i] < ' ')
			snprintf(buf, sizeof(buf), "C-%c ", bytes[i] + '`');
		else
			snprintf(buf, sizeof(buf), "%c ", bytes[i]);
		emil_strlcat(seqR, buf, sizeof(seqR));
	}
	setStatusMessage("Unknown command M-%s", seqR);
	return 033;
}

/* Raw reading a keypress - terminal layer only handles raw byte
 * reading, escape sequence decoding, and UTF-8 assembly.
 * Returns key tokens only: no binding policy. */
int readKey(void) {
	/* Repair terminal/screen state after an asynchronous signal. */
	handlePendingSignals();

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
	while ((nread = read(STDIN_FILENO, &c, 1)) != 1) {
		if (nread == -1 && errno == EINTR) {
			/* Repair before yielding to the caller: every
			 * read loop treats -1 as "retry", and a retry
			 * into a cooked-mode terminal is what produced
			 * the literal ^G after C-z / fg. */
			handlePendingSignals();
			return -1;
		}
		if (nread == -1 && errno != EAGAIN)
			die("read");
	}
	if (c == 033) {
		uint8_t seen[ESC_SEEN_MAX];
		int n_seen;
		int key = decodeEscapeSequence(terminalEscByte, seen, &n_seen);
		if (key == 033)
			/* n_seen == 0 means the Meta-prefix wait was
			 * abandoned by a signal, not that a sequence was
			 * unrecognized: return the bare token silently
			 * rather than posting "Unknown command M-" with
			 * nothing after it. */
			return n_seen ? unknownEscape(seen, n_seen) : 033;
		return key;
	} else if (utf8_is2Char(c)) {
		E.nunicode = 2;
		E.unicode[0] = c;
		if (!terminalContByte(&E.unicode[1]))
			return KEY_UNICODE_ERROR;
		if (!utf8_validate(E.unicode, 2))
			return KEY_UNICODE_ERROR;
		return KEY_UNICODE;
	} else if (utf8_is3Char(c)) {
		E.nunicode = 3;
		E.unicode[0] = c;
		if (!terminalContByte(&E.unicode[1]))
			return KEY_UNICODE_ERROR;
		if (!terminalContByte(&E.unicode[2]))
			return KEY_UNICODE_ERROR;
		if (!utf8_validate(E.unicode, 3))
			return KEY_UNICODE_ERROR;
		return KEY_UNICODE;
	} else if (utf8_is4Char(c)) {
		E.nunicode = 4;
		E.unicode[0] = c;
		if (!terminalContByte(&E.unicode[1]))
			return KEY_UNICODE_ERROR;
		if (!terminalContByte(&E.unicode[2]))
			return KEY_UNICODE_ERROR;
		if (!terminalContByte(&E.unicode[3]))
			return KEY_UNICODE_ERROR;
		if (!utf8_validate(E.unicode, 4))
			return KEY_UNICODE_ERROR;
		return KEY_UNICODE;
	}
	return c;
}
