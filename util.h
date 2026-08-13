/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
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

/* Copy src into dst[dsize], doubling each '%' so the result can be
 * safely embedded in a printf-style format string.  Use whenever
 * user-controlled text is interpolated into an editorPrompt() format
 * or any other string later passed as a format. */

/* Tilde / home-directory helpers */
char *expandTilde(const char *path);  /* ~/foo → /home/u/foo; caller frees */
char *collapseHome(const char *path); /* /home/u/foo → ~/foo; caller frees */

/* write(2) that does not stop part-way.  Returns 0 when every byte was
 * written, -1 otherwise with errno set.  See util.c for why the plain
 * write() this replaces was not enough. */
int writeAll(int fd, const void *buf, size_t len);

/* Character classification */
int isWordBoundary(uint8_t c);

#endif /* EMIL_UTIL_H */
