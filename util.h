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

/* Character classification */
int isWordBoundary(uint8_t c);

#endif /* EMIL_UTIL_H */
