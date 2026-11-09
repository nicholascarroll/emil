/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include "undorecord.h"
#include "util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

struct undo *newUndo(void) {
	struct undo *ret = xmalloc(sizeof(*ret));
	ret->prev = NULL;
	ret->paired = 0;
	ret->startx = 0;
	ret->starty = 0;
	ret->endx = 0;
	ret->endy = 0;
	ret->append = 0; /* the mutation layer opts in; see pushUndo */
	ret->nmerges = 0;
	ret->uncapped = 0;
	ret->delete = 0;
	ret->pointx = 0;
	ret->pointy = -1; /* not recorded */
	ret->datalen = 0;
	ret->datasize = 22;
	ret->data = xmalloc(ret->datasize);
	ret->data[0] = 0;
	return ret;
}

struct undo *undoRecordNew(int startx, int starty, int endx, int endy,
			   int is_delete, const uint8_t *data, int datalen) {
	struct undo *u = newUndo();
	u->startx = startx;
	u->starty = starty;
	u->endx = endx;
	u->endy = endy;
	u->delete = is_delete;
	undoRecordReserve(u, datalen);
	if (datalen > 0)
		memcpy(u->data, data, datalen);
	u->data[datalen] = 0;
	u->datalen = datalen;
	return u;
}

void undoRecordReserve(struct undo *u, int needed) {
	if (needed < 0 || needed == INT_MAX)
		abort();
	if (needed + 1 <= u->datasize)
		return;
	int newsize = u->datasize ? u->datasize : 22;
	while (newsize < needed + 1) {
		if (newsize > INT_MAX / 2) {
			newsize = needed + 1;
			break;
		}
		newsize *= 2;
	}
	u->data = xrealloc(u->data, newsize);
	u->datasize = newsize;
}

void undoRecordFreeList(struct undo *first) {
	struct undo *cur = first;
	struct undo *prev;

	while (cur != NULL) {
		free(cur->data);
		prev = cur;
		cur = prev->prev;
		free(prev);
	}
}
