#ifndef EMIL_PROMPT_H
#define EMIL_PROMPT_H

#include <stdint.h>
#include "emil.h"

/* Main prompt function for minibuffer input */
uint8_t *editorPrompt(struct editorBuffer *bufr, uint8_t *prompt,
		      enum promptType t,
		      void (*callback)(struct editorBuffer *, uint8_t *, int));

#endif /* EMIL_PROMPT_H */
