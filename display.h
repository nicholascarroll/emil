#ifndef EMIL_DISPLAY_H
#define EMIL_DISPLAY_H

#include "abuf.h"
#include "window.h"
#include <stddef.h>

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
void drawStatusBar(struct window *win, struct abuf *ab, int line);
void drawMinibuffer(struct abuf *ab);
void scroll(void);
void scrollViewport(struct window *win, struct buffer *buf, int n);
void clampCursorToViewport(struct window *win, struct buffer *buf);
void setScxScy(struct window *win);
void cursorBottomLine(int curs);
void resizeScreen(int sig);
void recenter(struct window *win);
void toggleVisualLineMode(void);
void editorVersion(void);
void editorStatus(void);
void whatCursor(void);

#endif /* EMIL_DISPLAY_H */
