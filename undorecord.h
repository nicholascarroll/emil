/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_UNDORECORD_H
#define EMIL_UNDORECORD_H 1

#include "emil.h"
#include <stdint.h>

/* Creating and freeing undo records.  A record owns its data; linking it
 * into a list is pushUndo's job. */

/* A record with no data and every flag clear, and the point not recorded. */
struct undo *newUndo(void);

/* A record of a change to [start, end] whose text is a copy of 'data'. */
struct undo *undoRecordNew(int startx, int starty, int endx, int endy,
			   int is_delete, const uint8_t *data, int datalen);

/* Grow u->data to hold at least 'needed' bytes plus a NUL. */
void undoRecordReserve(struct undo *u, int needed);

/* Free a record and every record reached through its 'prev'. */
void undoRecordFreeList(struct undo *first);

#endif
