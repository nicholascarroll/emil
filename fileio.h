/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_FILEIO_H
#define EMIL_FILEIO_H

#include <stddef.h>

struct buffer;
struct config;

/* File locking.
 *
 * lockFile distinguishes a conflict from an error, because the two
 * want different responses from the background re-probe: a conflict
 * is worth waiting out, an error is not.  ENOLCK in particular is
 * what an NFS mount without a running rpc.lockd returns -- there is
 * no holder and never will be one, so retrying every two seconds for
 * the rest of the session accomplishes nothing. */
enum lockResult {
	LOCK_ACQUIRED = 0,
	LOCK_CONFLICT = -1,    /* held by another process; retry */
	LOCK_RETRY = -2,       /* interrupted (EINTR); retry */
	LOCK_UNAVAILABLE = -3, /* ENOENT, ENOLCK, ...; never retry */
};

int probeLock(const char *filename);
int lockFile(struct buffer *bufr, const char *filename);
void releaseLock(struct buffer *bufr);

/* Re-assert every advisory lock emil believes it holds.  Must be
 * called after any operation that opens and closes a file, because
 * closing any descriptor on an inode drops every lock this process
 * holds on it -- see the comment on the definition in fileio.c. */
void relockAll(void);
void checkFileModified(void);
void initFileCheck(void);
void resetFileCheckThrottle(void);

/* File I/O operations */
char *rowsToString(struct buffer *bufr, size_t *buflen);
int editorOpen(struct buffer *bufr, const char *filename);
void save(void);
void saveAs(void);
void revert(void);
void findFile(int read_only);
struct buffer *switchToFile(const char *filename);
void insertFile(void);

/* Body of insert-file split from the prompt.  Loads `path`, validates
 * it (directory check, size budget, binary rejection, UTF-8), and
 * inserts the contents at the current point of `buf` through the
 * mutation layer (so the insertion is undoable as a single unit).
 * Returns 0 on success, non-zero on any failure (status message set).
 *
 * `display_name` is what the user typed — shown in the status line;
 * may differ from `path` if tilde expansion happened.  May be NULL;
 * in that case `path` is used for display too. */
int insertFileAtPath(struct buffer *buf, const char *path,
		     const char *display_name);
void changeDirectory(void);

char *relativePath(const char *from, const char *to);
char *cleanPath(char *path);
char *absolutePath(const char *path); /* resolve to absolute; caller frees */
char *rebaseFilename(const char *filename, const char *old_cwd,
		     const char *new_cwd);

/* Stdin loading */
char *readAllFromFd(int fd, size_t *out_len);
struct buffer *loadStdinBuffer(const char *data, size_t len);

#endif /* EMIL_FILEIO_H */
