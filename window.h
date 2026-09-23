/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
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

/* NULL when the focus invariant (see E.buf in emil.h) holds, else a
 * short description of how it is broken. */
const char *focusInvariantBreach(void);

#endif /* EMIL_WINDOW_H */
