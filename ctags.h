/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
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
 * filesystem root for a "tags" file.
 * writes the absolute directory containing it (no trailing slash, "/"
 * for root) into out_dir and returns 0.  Returns -1 if no tags file is
 * found before the root or if out_dir is too small. Exposed for testing.*/
int findTagsDir(char *out_dir, size_t dirsz);

/* Resolve a path taken from a tags file (which is relative to the
 * directory containing that tags file) into a path suitable for
 * opening.  Absolute ("/...") and home-relative ("~...") paths are
 * copied through unchanged; anything else is joined onto tagsdir.
 * Returns 0 on success, -1 if the result would not fit in out.
 * Exposed for testing. */
int resolveTagPath(const char *tagsdir, const char *tagpath, char *out,
		   size_t outsz);

/* One tags-file entry.  file is the path as the tags file writes it,
 * path the same joined onto the tags directory.  line is 0 when the
 * entry gives no line number (neither a numeric address nor a line:
 * field); pat is the search pattern without delimiters or anchors, ""
 * when there is none.  scope is the enclosing class, struct, namespace
 * or the like, NULL if the entry names none. */
struct tagMatch {
	char *file;
	char *path;
	char *pat;
	char *scope;
	int line;
};

/* Parse one line of a tags file.  Returns 0 if it is an entry for sym
 * and fills m with pointers into line, which is modified; path is left
 * NULL.  Returns -1 for any other line.  Exposed for testing. */
int ctagsParseLine(char *line, const char *sym, struct tagMatch *m);

/* The word at point for a tags lookup, or NULL if there is none: a run
 * of ASCII identifier characters and non-ASCII letters, ended by
 * anything else -- including punctuation and U+200B ZERO WIDTH SPACE,
 * which marks word boundaries in scripts written without spaces.
 * Caller frees.  Exposed for testing. */
char *ctagsWordAtPoint(void);

#endif /* EMIL_CTAGS_H */
