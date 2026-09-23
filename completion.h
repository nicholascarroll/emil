/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_COMPLETION_H
#define EMIL_COMPLETION_H

#include "emil.h"

void resetCompletionState(struct completionState *state);
void handleMinibufferCompletion(struct buffer *minibuf, enum promptType type);
void cycleCompletion(struct buffer *minibuf, int direction);

/* TAB in the shell prompt (#131): complete the word ending at point,
 * as an executable on PATH in command position and as a file name
 * elsewhere.  Inserts at point only, quoted for the quote open there;
 * a unique match that is not a directory also closes that quote and
 * adds a space.  A second TAB with nothing to add lists the matches. */
void handleShellCompletion(struct buffer *minibuf);
void closeCompletionsBuffer(void);
void replaceMinibufferText(struct buffer *minibuf, const char *text);

#endif
