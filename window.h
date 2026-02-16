#ifndef WINDOW_H
#define WINDOW_H

#include "emil.h"

int windowFocusedIdx(void);
int findBufferWindow(struct editorBuffer *buf);
void synchronizeBufferCursor(struct editorBuffer *buf,
			     struct editorWindow *win);
void editorSwitchWindow(void);
void editorCreateWindow(void);
void editorDestroyWindow(int window_idx);
void editorDestroyOtherWindows(void);

#endif /* WINDOW_H */
