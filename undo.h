/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#ifndef EMIL_UNDO_H
#define EMIL_UNDO_H 1

#include "dbuf.h"
#include "emil.h"
#include "undorecord.h"

/* Compute the position reached after inserting 'len' bytes of 'text'
 * starting at (startx, starty).  Pure. */
void computeInsertEnd(const uint8_t *text, int len, int startx, int starty,
		      int *endx, int *endy);

enum undoResult {
	UNDO_DONE,	/* every requested step was taken */
	UNDO_EXHAUSTED, /* history ran out first */
	UNDO_REFUSED,	/* read-only buffer, already reported */
	UNDO_MISMATCH,	/* the next record doesn't match the text: not
			 * applied, and reported */
};

enum undoResult doUndo(struct buffer *buf, int count);
enum undoResult doRedo(struct buffer *buf, int count);

enum undoMergeShape {
	UNDO_MERGE_NONE,
	UNDO_MERGE_APPEND,  /* new's data goes after prev's */
	UNDO_MERGE_PREPEND, /* new's data goes before prev's */
};

/* Would 'new' fold into 'prev', the head of the undo list?  Changes
 * nothing.  pushUndo calls this only for a record with 'append' set. */
enum undoMergeShape undoMergeDecision(const struct undo *prev,
				      const struct undo *new);

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

/* Is (x, y) a position in the text?  A position may be at any byte of a
 * row or just after its last. */
int bufferPosValid(const struct buffer *buf, int x, int y);

/* Are both ends positions in the text, with start not after end? */
int bufferRangeValid(const struct buffer *buf, int startx, int starty, int endx,
		     int endy);

/* Change the rows and adjust points; neither records undo.  Both return
 * 0, or -1 with nothing changed if the position or range is invalid. */
int bulkInsert(struct buffer *buf, int startx, int starty, const uint8_t *data,
	       int datalen);
int bulkDelete(struct buffer *buf, int startx, int starty, int endx, int endy);

#endif
