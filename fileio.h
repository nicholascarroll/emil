#ifndef EMIL_FILEIO_H
#define EMIL_FILEIO_H

#include <stddef.h>

struct buffer;
struct config;

/* File locking */
int lockFile(struct buffer *bufr, const char *filename);
void releaseLock(struct buffer *bufr);
void checkFileModified(void);

/* File I/O operations */
char *rowsToString(struct buffer *bufr, size_t *buflen);
int editorOpen(struct buffer *bufr, char *filename);
void save(void);
void saveAs(void);
void revert(void);
void findFile(void);
struct buffer *switchToFile(const char *filename);
void insertFile(void);
void changeDirectory(void);

char *relativePath(const char *from, const char *to);
char *cleanPath(char *path);
char *rebaseFilename(const char *filename, const char *old_cwd,
		     const char *new_cwd);

/* Stdin loading */
char *readAllFromFd(int fd, size_t *out_len);
struct buffer *loadStdinBuffer(const char *data, size_t len);
#endif /* EMIL_FILEIO_H */
