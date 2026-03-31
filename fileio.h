#ifndef EMIL_FILEIO_H
#define EMIL_FILEIO_H

#include <stddef.h>

struct buffer;
struct config;

/* File locking */
int probeLock(const char *filename);
int lockFile(struct buffer *bufr, const char *filename);
void releaseLock(struct buffer *bufr);
void checkFileModified(void);

/* File I/O operations */
char *rowsToString(struct buffer *bufr, size_t *buflen);
int editorOpen(struct buffer *bufr, char *filename);
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
char *rebaseFilename(const char *filename, const char *old_cwd,
		     const char *new_cwd);

/* Stdin loading */
char *readAllFromFd(int fd, size_t *out_len);
struct buffer *loadStdinBuffer(const char *data, size_t len);
#endif /* EMIL_FILEIO_H */
