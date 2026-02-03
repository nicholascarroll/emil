#ifndef FILEIO_H
#define FILEIO_H

struct editorBuffer;
struct editorConfig;

/* File I/O operations */
char *editorRowsToString(struct editorBuffer *bufr, int *buflen);
int editorOpen(struct editorBuffer *bufr, char *filename);
void editorSave(struct editorBuffer *bufr);
void editorRevert(struct editorConfig *ed, struct editorBuffer *buf);
void findFile(void);
void editorInsertFile(struct editorConfig *ed, struct editorBuffer *buf);
void editorSaveAs(struct editorBuffer *bufr);
#endif /* FILEIO_H */
