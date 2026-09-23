/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_PROMPT_H
#define EMIL_PROMPT_H

#include <stdint.h>
#include "emil.h"

/* Read a line in the minibuffer.  Returns it malloc'd, or NULL on C-g
 * or when a prompt is already open.
 *
 * callback, if not NULL, runs after every key with the buffer that was
 * current when the prompt opened -- not E.buf, which is the minibuffer
 * while the prompt runs -- the text so far, and the key. */
uint8_t *editorPrompt(const char *prompt, enum promptType t,
		      void (*callback)(struct buffer *, uint8_t *, int));

/* Serialize minibuffer rows, joining them with sep: "\n" for the value
 * returned to a caller, "^J" for display.  Returns a malloc'd string. */
char *minibufJoin(struct buffer *mb, const char *sep);

/* Copy 's' rewriting each '\n' as "^J".  Required whenever user text
 * that may contain a literal newline is embedded into a prompt prefix
 * or status message: E.statusmsg is drawn raw, so an embedded 0x0A
 * executes as a line feed and breaks the minibuffer display.  Returns
 * a malloc'd string; caller frees. */
char *caretEscapeNewlines(const uint8_t *s);

#endif /* EMIL_PROMPT_H */
