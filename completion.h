#ifndef EMIL_COMPLETION_H
#define EMIL_COMPLETION_H

#include "emil.h"

void resetCompletionState(struct completion_state *state);
void handleMinibufferCompletion(struct buffer *minibuf, enum promptType type);
void cycleCompletion(struct buffer *minibuf, int direction);
void closeCompletionsBuffer(void);

/* Replace the minibuffer's contents with `text`, leaving point at the
 * end of the inserted text on row 0.  The minibuffer is always a
 * single-row buffer, so this is the canonical way to overwrite it. */
void replaceMinibufferText(struct buffer *minibuf, const char *text);

#endif
