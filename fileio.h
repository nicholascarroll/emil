#ifndef EMIL_FILEIO_H
#define EMIL_FILEIO_H

struct buffer;
struct config;

/* File locking */
int lockFile(struct buffer *bufr, const char *filename);
void releaseLock(struct buffer *bufr);
void checkFileModified(void);

/* File I/O operations */
char *rowsToString(struct buffer *bufr, int *buflen);
int editorOpen(struct buffer *bufr, char *filename);
void save(void);
void saveAs(void);
void revert(void);
void findFile(void);
struct buffer *switchToFile(const char *filename);
void insertFile(void);
void changeDirectory(void);

char *relativePath(const char *from, const char *to);
char *rebaseFilename(const char *filename, const char *old_cwd,
		     const char *new_cwd);
#endif /* EMIL_FILEIO_H */
