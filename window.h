/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_WINDOW_H
#define EMIL_WINDOW_H

#include "emil.h"

/* How the screen's rows are shared: each window's height plus its
 * status bar, and the message line below them all.  The display sets
 * minibuffer_height each frame to fit the message. */
extern int minibuffer_height;
extern const int statusbar_height;

int windowFocusedIdx(void);
int findBufferWindow(struct buffer *buf);
void synchronizeBufferCursor(struct buffer *buf, struct window *win);
void switchWindow(void);
void resetWindowHeights(void);
void createWindow(void);
void destroyWindow(int window_idx);
void destroyOtherWindows(void);
void showPopupBuffer(struct buffer *buf);

/* NULL if the focus invariant (E.buf in emil.h) holds, else what broke. */
const char *focusInvariantBreach(void);

#endif /* EMIL_WINDOW_H */
