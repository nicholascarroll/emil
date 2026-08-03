/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#ifndef EMIL_UNDO_H
#define EMIL_UNDO_H 1

#include "dbuf.h"
#include "emil.h"

/* Compute the position reached after inserting 'len' bytes of 'text'
 * starting at (startx, starty).  Pure. */
void computeInsertEnd(const uint8_t *text, int len, int startx, int starty,
		      int *endx, int *endy);

void doUndo(struct buffer *buf, int count);
void doRedo(struct buffer *buf, int count);
struct undo *newUndo(void);
void undoReplaceData(struct undo *u, int newsize);

/* Add a record to the undo list, taking ownership.  If the record has
 * 'append' set it may be folded into the run at the head of the list,
 * in which case it is freed.  Otherwise it closes that run and becomes
 * the new head.  Setting 'append' is the mutation layer's call. */
void pushUndo(struct buffer *buf, struct undo *new);

/* Close the open run, if any, so the next edit starts a fresh
 * record. */
void undoCloseRun(struct buffer *buf);

void clearRedos(struct buffer *buf);
void clearUndosAndRedos(struct buffer *buf);
void bulkInsert(struct buffer *buf, int startx, int starty, const uint8_t *data,
		int datalen);
void bulkDelete(struct buffer *buf, int startx, int starty, int endx, int endy);

#endif
