#ifndef EMIL_COMPLETION_H
#define EMIL_COMPLETION_H

#include "emil.h"

void resetCompletionState(struct completion_state *state);
void handleMinibufferCompletion(struct buffer *minibuf, enum promptType type);
void cycleCompletion(struct buffer *minibuf, int direction);
void closeCompletionsBuffer(void);

#endif
