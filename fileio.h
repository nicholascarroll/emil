#ifndef FILEIO_H
#define FILEIO_H

struct editorBuffer;
struct editorConfig;

/* File locking */
int editorLockFile(struct editorBuffer *bufr, const char *filename);
void editorReleaseLock(struct editorBuffer *bufr);
void editorCheckFileModified(struct editorBuffer *bufr);

/* File I/O operations */
char *editorRowsToString(struct editorBuffer *bufr, int *buflen);
int editorOpen(struct editorBuffer *bufr, char *filename);
void editorSave(struct editorBuffer *bufr);
void editorSaveAs(struct editorBuffer *bufr);
void editorRevert(struct editorConfig *ed, struct editorBuffer *buf);
void findFile(void);
struct editorBuffer *editorSwitchToFile(const char *filename);
void editorInsertFile(struct editorConfig *ed, struct editorBuffer *buf);
void editorChangeDirectory(struct editorConfig *ed, struct editorBuffer *buf);

char *relativePath(const char *from, const char *to);
char *rebaseFilename(const char *filename, const char *old_cwd,
		     const char *new_cwd);
#endif /* FILEIO_H */
