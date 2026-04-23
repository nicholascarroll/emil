#include "window.h"
#include "display.h"
#include "emil.h"
#include "message.h"
#include "util.h"
#include <stdlib.h>

extern struct config E;

int windowFocusedIdx(void) {
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->focused) {
			return i;
		}
	}
	return 0;
}

int findBufferWindow(struct buffer *buf) {
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->buf == buf) {
			return i;
		}
	}
	return -1;
}

void synchronizeBufferCursor(struct buffer *buf, struct window *win) {
	// Ensure the cursor is within the buffer's bounds
	if (win->cy >= buf->numrows) {
		win->cy = buf->numrows > 0 ? buf->numrows - 1 : 0;
	}
	if (win->cy < buf->numrows && win->cx > buf->row[win->cy].size) {
		win->cx = buf->row[win->cy].size;
	}

	// Update the buffer's cursor position
	buf->cx = win->cx;
	buf->cy = win->cy;
}

void switchWindow(void) {
	if (E.nwindows == 1) {
		setStatusMessage(msg_no_other_windows);
		return;
	}

	int currentIdx = windowFocusedIdx();
	struct window *currentWindow = E.windows[currentIdx];
	struct buffer *currentBuffer = currentWindow->buf;

	// Store the current buffer's cursor position in the current window
	currentWindow->cx = currentBuffer->cx;
	currentWindow->cy = currentBuffer->cy;

	// Switch to the next window
	currentWindow->focused = 0;
	int nextIdx = (currentIdx + 1) % E.nwindows;
	struct window *nextWindow = E.windows[nextIdx];
	nextWindow->focused = 1;

	// Update the focused buffer
	E.buf = nextWindow->buf;

	// Set the buffer's cursor position from the new window
	E.buf->cx = nextWindow->cx;
	E.buf->cy = nextWindow->cy;

	// Synchronize the buffer's cursor with the new window's cursor
	synchronizeBufferCursor(E.buf, nextWindow);
}

void createWindow(void) {
	E.windows =
		xrealloc(E.windows, sizeof(struct window *) * (++E.nwindows));
	E.windows[E.nwindows - 1] = xcalloc(1, sizeof(struct window));
	E.windows[E.nwindows - 1]->focused = 0;
	E.windows[E.nwindows - 1]->buf = E.buf;
	E.windows[E.nwindows - 1]->cx = E.buf->cx;
	E.windows[E.nwindows - 1]->cy = E.buf->cy;
	E.windows[E.nwindows - 1]->rowoff = 0;
	E.windows[E.nwindows - 1]->coloff = 0;

	// Force all windows to recalculate heights
	for (int i = 0; i < E.nwindows; i++) {
		E.windows[i]->height = 0;
	}
}

void destroyWindow(int window_idx) {
	if (E.nwindows == 1) {
		setStatusMessage(msg_cant_kill_last_window);
		return;
	}

	int focused_idx = windowFocusedIdx();

	/* switch focus before destroying current window */
	if (window_idx == focused_idx) {
		switchWindow();
	}

	free(E.windows[window_idx]);
	struct window **windows =
		xmalloc(sizeof(struct window *) * (--E.nwindows));
	int j = 0;
	for (int i = 0; i < E.nwindows + 1; i++) {
		if (i != window_idx) {
			windows[j] = E.windows[i];
			j++;
		}
	}
	free(E.windows);
	E.windows = windows;

	/* reset heights */
	for (int i = 0; i < E.nwindows; i++) {
		E.windows[i]->height = 0;
	}
}

void destroyOtherWindows(void) {
	if (E.nwindows == 1) {
		setStatusMessage(msg_no_windows_delete);
		return;
	}
	int idx = windowFocusedIdx();
	struct window **windows = xmalloc(sizeof(struct window *));
	for (int i = 0; i < E.nwindows; i++) {
		if (i != idx) {
			free(E.windows[i]);
		}
	}
	windows[0] = E.windows[idx];
	windows[0]->focused = 1;
	E.buf = windows[0]->buf;
	E.nwindows = 1;
	free(E.windows);
	E.windows = windows;

	resizeScreen(0);
}

/* Resize windows so that the popup at win_idx fits its content while
 * the remaining windows share the leftover space fairly. */
static void sizePopupWindow(int win_idx) {
	extern int minibuffer_height;
	extern const int statusbar_height;

	struct buffer *buf = E.windows[win_idx]->buf;
	int total_height = E.screenrows - minibuffer_height -
			   (statusbar_height * E.nwindows);
	int non_popup = E.nwindows - 1;

	int popup_height = buf->numrows + 2;
	int max_popup = total_height - non_popup * 3;
	if (popup_height > max_popup)
		popup_height = max_popup;
	if (popup_height < 3)
		popup_height = 3;

	int remaining = total_height - popup_height;
	int per_win = non_popup > 0 ? remaining / non_popup : remaining;

	for (int i = 0; i < E.nwindows; i++)
		E.windows[i]->height = (i == win_idx) ? popup_height : per_win;
}

void showPopupBuffer(struct buffer *buf) {
	int win_idx = findBufferWindow(buf);
	if (win_idx < 0) {
		/* First time: create the window and keep focus
		 * on the original (first) window. */
		win_idx = E.nwindows;
		createWindow();
		E.windows[win_idx]->buf = buf;
		E.windows[win_idx]->focused = 0;
		for (int i = 0; i < E.nwindows; i++)
			E.windows[i]->focused = (i == 0);
	}

	/* Always resize to match the current content. */
	sizePopupWindow(win_idx);
}
