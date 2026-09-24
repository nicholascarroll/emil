/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_PROMPT_H
#define EMIL_PROMPT_H

#include <stdint.h>
#include "emil.h"

/* Read a line in the minibuffer; malloc'd, or NULL on C-g or if a
 * prompt is already open.  callback, if given, runs after each key with
 * the buffer the prompt was opened from, the text so far, and the key. */
uint8_t *editorPrompt(const char *prompt, enum promptType t,
		      void (*callback)(struct buffer *, uint8_t *, int));

/* Serialize minibuffer rows, joining them with sep: "\n" for the value
 * returned to a caller, "^J" for display.  Returns a malloc'd string. */
char *minibufJoin(struct buffer *mb, const char *sep);

#endif /* EMIL_PROMPT_H */
