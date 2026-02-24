#ifndef EMIL_COMPLETION_H
#define EMIL_COMPLETION_H

#include "emil.h"

void resetCompletionState(struct completion_state *state);
void handleMinibufferCompletion(struct editorBuffer *minibuf,
				enum promptType type);
void cycleCompletion(struct editorBuffer *minibuf, int direction);
void closeCompletionsBuffer(void);

#endif
