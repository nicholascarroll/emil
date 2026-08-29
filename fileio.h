/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_FILEIO_H
#define EMIL_FILEIO_H

#include <stddef.h>
#include <fcntl.h>

struct buffer;
struct config;

/* POSIX advisory record locking is not universally available.  WASIX
 * (and wasi-libc generally) declares struct flock but none of the
 * locking constants, because there is no host lock manager behind
 * them: F_GETLK, F_SETLK, F_SETLKW, the F_RDLCK/F_WRLCK/F_UNLCK
 * l_type values and the F_OFD_* commands all sit inside the
 * __wasilibc_unmodified_upstream guards in its <fcntl.h>.  Detect that
 * by the absence of the command constants rather than by testing for a
 * specific platform macro, so any libc making the same choice is
 * handled without further edits here.
 *
 * Testing two constants to guard code that needs six is sound only
 * because they are absent together, which is a property of that header
 * rather than of C.  It holds on the pinned sysroot; a libc that
 * declared the l_type values without the commands would break the
 * build rather than misbehave, which is the failure direction to want.
 *
 * This lives in the header rather than in fileio.c because the tests
 * need the same answer.  test_warnings.c used to reach for F_WRLCK
 * directly and simply failed to compile on WASIX, which told us
 * nothing about the editor; it now asserts the contract below. */
#if !defined(F_GETLK) || !defined(F_SETLK)
#define EMIL_NO_FILE_LOCKING 1
#endif

/* File locking.
 *
 * Where EMIL_NO_FILE_LOCKING is set the contract is: probeLock() always
 * reports 0 (nothing held), lockFile() always returns LOCK_UNAVAILABLE
 * and never sets bufr->lock_fd, releaseLock() and relockIfDirty() are
 * no-ops, and the editor opens, edits and saves exactly as it otherwise
 * would -- it simply cannot warn about a rival holding the file.
 *
 * ENOLCK is what an NFS mount without a running rpc.lockd returns.*/
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
void relockIfDirty(struct buffer *bufr);
void checkFileModified(void);
void initFileCheck(void);
void resetFileCheckThrottle(void);

/* File I/O operations */
char *rowsToString(struct buffer *bufr, size_t *buflen);
int editorOpen(struct buffer *bufr, const char *filename);
void save(int uarg);
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
