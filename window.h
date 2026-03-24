#ifndef EMIL_WINDOW_H
#define EMIL_WINDOW_H

#include "emil.h"

int windowFocusedIdx(void);
int findBufferWindow(struct buffer *buf);
void synchronizeBufferCursor(struct buffer *buf, struct window *win);
void switchWindow(void);
void createWindow(void);
void destroyWindow(int window_idx);
void destroyOtherWindows(void);
void showPopupBuffer(struct buffer *buf);

#endif /* EMIL_WINDOW_H */
