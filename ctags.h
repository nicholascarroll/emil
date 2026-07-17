#ifndef EMIL_CTAGS_H
#define EMIL_CTAGS_H

#include "emil.h"

/* CTags: jump to definition of word at point (M-.) */
void ctagsJump(void);

/* CTags: pop back to previous location (M-,) */
void ctagsBack(void);

/* Toggle between .c and .h file (M-/) */
void toggleHeaderBody(void);

/* Search from the current working directory upward toward the
 * filesystem root for a "tags" file (as vim/emacs do).  On success
 * writes the absolute directory containing it (no trailing slash, "/"
 * for root) into out_dir and returns 0.  Returns -1 if no tags file is
 * found before the root or if out_dir is too small.  Exposed for
 * testing and reuse. */
int findTagsDir(char *out_dir, size_t dirsz);

/* Resolve a path taken from a tags file (which is relative to the
 * directory containing that tags file) into a path suitable for
 * opening.  Absolute ("/...") and home-relative ("~...") paths are
 * copied through unchanged; anything else is joined onto tagsdir.
 * Returns 0 on success, -1 if the result would not fit in out.
 * Exposed for testing. */
int resolveTagPath(const char *tagsdir, const char *tagpath, char *out,
		   size_t outsz);

#endif /* EMIL_CTAGS_H */
