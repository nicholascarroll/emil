#ifndef EMIL_UTIL_H
#define EMIL_UTIL_H

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/types.h>

/* Memory allocation wrappers that abort on failure */
void *xmalloc(size_t size);
void *xrealloc(void *ptr, size_t size);
void *xcalloc(size_t nmemb, size_t size);
char *xstrdup(const char *s);

/* Portable getline implementation */
ssize_t emil_getline(char **lineptr, size_t *n, FILE *stream);

/* Safe string functions (BSD-style but portable) */
size_t emil_strlcpy(char *dst, const char *src, size_t dsize);
size_t emil_strlcat(char *dst, const char *src, size_t dsize);

/* Tilde / home-directory helpers */
char *expandTilde(const char *path);  /* ~/foo → /home/u/foo; caller frees */
char *collapseHome(const char *path); /* /home/u/foo → ~/foo; caller frees */
char *absolutePath(const char *path); /* resolve to absolute; caller frees */

/* Character classification */
int isWordBoundary(uint8_t c);

/* Memory budget: total bytes of text in all buffers + undo/redo data */
size_t totalBufferBytes(void);
size_t totalUndoBytes(void);
size_t totalBudgetBytes(void);

/* Set or clear E.memory_over_limit based on current budget.
 * Call after operations that may shrink the budget (close buffer,
 * revert-buffer, undo-chain truncation) and after operations that may grow
 * it (add to kill ring, insert text).  Setting is latching — once
 * set, the status-bar warning stays visible until the budget drops
 * back below the limit. */
void recheckMemoryBudget(void);

#endif /* EMIL_UTIL_H */
