/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_COMPLETION_H
#define EMIL_COMPLETION_H

#include "emil.h"

void resetCompletionState(struct completionState *state);
void handleMinibufferCompletion(struct buffer *minibuf, enum promptType type);
void cycleCompletion(struct buffer *minibuf, int direction);

/* TAB completion in the shell prompt. */
void handleShellCompletion(struct buffer *minibuf);
void closeCompletionsBuffer(void);
void replaceMinibufferText(struct buffer *minibuf, const char *text);

#endif
