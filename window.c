#include "window.h"
#include "emil.h"
#include "message.h"
#include "util.h"
#include <stdlib.h>

extern struct editorConfig E;

int windowFocusedIdx(void) {
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->focused) {
			return i;
		}
	}
	/* You're in trouble m80 */
	return 0;
}

int findBufferWindow(struct editorBuffer *buf) {
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->buf == buf) {
			return i;
		}
	}
	return -1;
}

void synchronizeBufferCursor(struct editorBuffer *buf,
			     struct editorWindow *win) {
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

void editorSwitchWindow(void) {
	if (E.nwindows == 1) {
		editorSetStatusMessage("No other windows to select");
		return;
	}

	int currentIdx = windowFocusedIdx();
	struct editorWindow *currentWindow = E.windows[currentIdx];
	struct editorBuffer *currentBuffer = currentWindow->buf;

	// Store the current buffer's cursor position in the current window
	currentWindow->cx = currentBuffer->cx;
	currentWindow->cy = currentBuffer->cy;

	// Switch to the next window
	currentWindow->focused = 0;
	int nextIdx = (currentIdx + 1) % E.nwindows;
	struct editorWindow *nextWindow = E.windows[nextIdx];
	nextWindow->focused = 1;

	// Update the focused buffer
	E.buf = nextWindow->buf;

	// Set the buffer's cursor position from the new window
	E.buf->cx = nextWindow->cx;
	E.buf->cy = nextWindow->cy;

	// Synchronize the buffer's cursor with the new window's cursor
	synchronizeBufferCursor(E.buf, nextWindow);
}

void editorCreateWindow(void) {
	E.windows = xrealloc(E.windows,
			     sizeof(struct editorWindow *) * (++E.nwindows));
	E.windows[E.nwindows - 1] = xcalloc(1, sizeof(struct editorWindow));
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

void editorDestroyWindow(int window_idx) {
	if (E.nwindows == 1) {
		editorSetStatusMessage("Can't kill last window");
		return;
	}

	int focused_idx = windowFocusedIdx();

	/* switch focus before destroying current window */
	if (window_idx == focused_idx) {
		editorSwitchWindow();
	}

	free(E.windows[window_idx]);
	struct editorWindow **windows =
		xmalloc(sizeof(struct editorWindow *) * (--E.nwindows));
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

void editorDestroyOtherWindows(void) {
	if (E.nwindows == 1) {
		editorSetStatusMessage("No other windows to delete");
		return;
	}
	int idx = windowFocusedIdx();
	struct editorWindow **windows = xmalloc(sizeof(struct editorWindow *));
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
}
