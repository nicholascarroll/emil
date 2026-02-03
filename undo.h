#ifndef EMIL_UNDO_H
#define EMIL_UNDO_H 1

#include "emil.h"

void editorDoUndo(struct editorBuffer *buf, int count);
void editorDoRedo(struct editorBuffer *buf, int count);
void editorUndoAppendChar(struct editorBuffer *buf, uint8_t c);
void editorUndoAppendUnicode(struct editorConfig *ed, struct editorBuffer *buf);
void editorUndoBackSpace(struct editorBuffer *buf, uint8_t c);
void editorUndoDelChar(struct editorBuffer *buf, erow *row);
struct editorUndo *newUndo(void);
void clearRedos(struct editorBuffer *buf);
void clearUndosAndRedos(struct editorBuffer *buf);
#ifdef EMIL_DEBUG_UNDO
void debugUnpair(struct editorConfig *ed, struct editorBuffer *buf);
#endif
#endif
