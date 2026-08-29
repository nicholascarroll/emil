/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_DISPLAY_H
#define EMIL_DISPLAY_H

#include "abuf.h"
#include "window.h"
#include <stddef.h>
#include <stdarg.h>

/* Forward declarations */
struct window;
struct buffer;
struct config;

/* Display constants */
extern int minibuffer_height;
extern const int statusbar_height;

/* Minibuffer layout (§5.5, #117 R3).  One model, run by the sizing in
 * refreshScreen, by drawMinibuffer and by cursorBottomLine; they
 * disagreed while each had its own.  Auto-sizing is capped at
 * MINIBUF_MAX_LINES screen lines. */
#define MINIBUF_MAX_LINES 5

struct minibufLine {
	int start; /* byte offset of this line's first character */
	int end;   /* byte offset one past its last */
	int cols;  /* display columns the line's text occupies */
};

int minibufLayout(const char *msg, int prefix_cols, int screencols,
		  struct minibufLine *out, int max_lines);
int minibufHeightNeeded(void);

/* Display functions */
void refreshScreen(void);
void drawRows(struct window *win, struct abuf *ab, int screenrows,
	      int screencols);
void drawStatusBar(struct window *win, struct abuf *ab, int line,
		   int cursor_col);
void drawMinibuffer(struct abuf *ab);
int scroll(void);
void scrollViewport(struct window *win, struct buffer *buf, int n);
void scrollToShowCursor(struct window *win, struct buffer *buf);
void clampCursorToViewport(struct window *win, struct buffer *buf);
/* Where the focused cursor landed, as a by-product of placing the
 * viewport.  Filled by scrollFocused(); consumed by screenCursorPos(). */
struct cursorHint {
	int col; /* display column of (cx, cy) */
	int scx; /* screen column within the window */
	int scy; /* screen row within the window */
};

int scrollFocused(struct cursorHint *hint);
void screenCursorPos(struct window *win, const struct cursorHint *hint,
		     int *scx_out, int *scy_out);
void cursorBottomLine(int curs);
void resizeScreen(void);
void recenter(struct window *win);
void toggleVisualLineMode(void);
void editorVersion(void);
void help(void);
void describeChar(void);

/* Status message display */
void setStatusMessage(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
void clearStatusMessage(void);

#endif /* EMIL_DISPLAY_H */
