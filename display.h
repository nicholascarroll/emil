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
void screenCursorPos(struct window *win, int cursor_col, int *scx_out,
		     int *scy_out);
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
