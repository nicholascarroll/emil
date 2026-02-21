#ifndef DISPLAY_H
#define DISPLAY_H

#include <stddef.h>
#include "abuf.h"
#include "window.h"

/* Forward declarations */
struct editorWindow;
struct editorBuffer;
struct editorConfig;

/* Display constants */
extern const int minibuffer_height;
extern const int statusbar_height;

/* Display functions */
void refreshScreen(void);
void drawRows(struct editorWindow *win, struct abuf *ab, int screenrows,
	      int screencols);
void drawStatusBar(struct editorWindow *win, struct abuf *ab, int line);
void drawMinibuffer(struct abuf *ab);
void scroll(void);
void scrollViewport(struct editorWindow *win, struct editorBuffer *buf, int n);
void clampCursorToViewport(struct editorWindow *win, struct editorBuffer *buf);
void setScxScy(struct editorWindow *win);
void cursorBottomLine(int curs);
void editorResizeScreen(int sig);
void recenter(struct editorWindow *win);
void editorToggleVisualLineMode(void);
void editorVersion(void);
/* Wrappers for command table */
void editorVersionWrapper(struct editorConfig *ed, struct editorBuffer *buf);
void editorToggleVisualLineModeWrapper(struct editorConfig *ed,
				       struct editorBuffer *buf);
void editorWhatCursor(void);

#endif /* DISPLAY_H */
