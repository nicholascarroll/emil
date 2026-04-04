#ifndef EMIL_UNDO_H
#define EMIL_UNDO_H 1

#include "emil.h"

#define UNDO_LIMIT 100

void doUndo(struct buffer *buf, int count);
void doRedo(struct buffer *buf, int count);
void undoAppendChar(struct buffer *buf, uint8_t c);
void undoAppendUnicode(struct buffer *buf);
void undoBackSpace(struct buffer *buf, uint8_t c);
void undoDelChar(struct buffer *buf, erow *row);
struct undo *newUndo(void);
void undoReplaceData(struct undo *u, int newsize);
void pushUndo(struct buffer *buf, struct undo *new);
void clearRedos(struct buffer *buf);
void clearUndosAndRedos(struct buffer *buf);
void bulkInsert(struct buffer *buf, int startx, int starty, const uint8_t *data,
		int datalen);
void undoSelfInsert(uint8_t c, int count);

#ifdef EMIL_DEBUG_UNDO
void debugUnpair(void);
#endif
#endif
