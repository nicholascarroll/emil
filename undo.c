/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include "undo.h"
#include "adjust.h"
#include "buffer.h"
#include "dbuf.h"
#include "display.h"
#include "emil.h"

#include "region.h"
#include "unicode.h"
#include "util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void computeInsertEnd(const uint8_t *text, int len, int startx, int starty,
		      int *endx, int *endy) {
	int ex = startx, ey = starty;
	for (int i = 0; i < len; i++) {
		if (text[i] == '\n') {
			ey++;
			ex = 0;
		} else {
			ex++;
		}
	}
	*endx = ex;
	*endy = ey;
}

/* Perform the row-array mutation for an insert at (startx, starty).
 * Takes an ALREADY-ANCHORED position and payload, and does NOT adjust
 * tracked points — bulkInsert does that on the logical range.  Uses
 * direct memmove/memcpy and insertRow, no character-at-a-time
 * primitives.  Does NOT record undo. */
static void bulkInsertRaw(struct buffer *buf, int startx, int starty,
			  const uint8_t *data, int datalen) {
	if (datalen <= 0)
		return;

	/* numrows >= 1 and starty < numrows (#105), so the target row
	 * always exists.  A malformed record is a bug, not a case to
	 * paper over. */
	if (starty >= buf->numrows)
		return;

	/* Scan for newlines to decide single-line vs multi-line */
	const uint8_t *first_nl = memchr(data, '\n', datalen);

	if (first_nl == NULL) {
		/* Single-line insert: memmove tail right, memcpy data in */
		struct erow *row = &buf->row[starty];
		int needed = row->size + datalen + 1;
		rowEnsureCap(row, needed);
		memmove(&row->chars[startx + datalen], &row->chars[startx],
			row->size - startx + 1); /* +1 for NUL */
		memcpy(&row->chars[startx], data, datalen);
		row->size += datalen;
		row->cached_width = -1;
		row->cached_sublines = -1;
		markBufferDirty(buf);
		invalidateScreenCache(buf);
		return;
	}

	/* Multi-line insert.  Strategy:
	 *   1. Save the suffix of the start row (bytes after startx).
	 *   2. Truncate the start row at startx.
	 *   3. Append the first line fragment from data to the start row.
	 *   4. Insert complete interior lines as new rows.
	 *   5. Insert the last line fragment + saved suffix as a new row. */

	struct erow *row = &buf->row[starty];

	/* Save suffix */
	int suffix_len = row->size - startx;
	uint8_t *suffix = NULL;
	if (suffix_len > 0) {
		suffix = xmalloc(suffix_len);
		memcpy(suffix, &row->chars[startx], suffix_len);
	}

	/* Truncate start row at startx, then append first fragment */
	int first_frag_len = (int)(first_nl - data);
	int new_size = startx + first_frag_len;
	row->chars = xrealloc(row->chars, new_size + 1);
	row->charcap = new_size + 1;
	if (first_frag_len > 0)
		memcpy(&row->chars[startx], data, first_frag_len);
	row->size = new_size;
	row->chars[row->size] = '\0';
	row->cached_width = -1;
	row->cached_sublines = -1;

	/* Walk remaining data, inserting interior and final lines */
	int insert_at = starty + 1;
	const uint8_t *p = first_nl + 1; /* skip past first '\n' */
	const uint8_t *end = data + datalen;

	while (p < end) {
		const uint8_t *nl = memchr(p, '\n', end - p);
		if (nl == NULL) {
			/* Last fragment — combine with saved suffix */
			int last_frag_len = (int)(end - p);
			int combined_len = last_frag_len + suffix_len;
			uint8_t *combined = xmalloc(combined_len + 1);
			memcpy(combined, p, last_frag_len);
			if (suffix_len > 0)
				memcpy(&combined[last_frag_len], suffix,
				       suffix_len);
			combined[combined_len] = '\0';
			insertRow(buf, insert_at, combined, combined_len);
			free(combined);
			free(suffix);
			markBufferDirty(buf);
			invalidateScreenCache(buf);
			return;
		}
		/* Interior complete line */
		int line_len = (int)(nl - p);
		insertRow(buf, insert_at, p, line_len);
		insert_at++;
		p = nl + 1;
	}

	/* If data ended with '\n', we still need to insert the suffix
	 * as a new row */
	if (suffix_len > 0) {
		insertRow(buf, insert_at, suffix, suffix_len);
	} else {
		insertRow(buf, insert_at, (const uint8_t *)"", 0);
	}
	free(suffix);
	markBufferDirty(buf);
	invalidateScreenCache(buf);
}

/* Bulk-insert text from 'data' (length 'datalen') into 'buf' at the
 * LOGICAL buffer position (startx, starty) -- which may be the virtual
 * EOF line.  Does NOT record undo.  Calls adjustAllPoints internally.
 *
 * The mutation runs on the anchored position and payload, so that
 * inserting at the virtual EOF materialises exactly one row and the
 * result matches what the typed paths produce.  The point adjustment
 * runs on the LOGICAL range, because a mark sitting at the end of the
 * last row -- which is precisely where the anchor lands -- must not be
 * dragged along by an insertion that happens after it.  adjustPoint's
 * insert branch treats a point exactly at startx as being after the
 * insertion, so handing it the anchored range would move that mark.
 *
 * Records name the insertion's own coordinates, so replaying
 * one through here is the identity translation and adjusts on the
 * range the record states. */
void bulkInsert(struct buffer *buf, int startx, int starty, const uint8_t *data,
		int datalen) {
	if (datalen <= 0)
		return;

	int log_endx, log_endy;
	computeInsertEnd(data, datalen, startx, starty, &log_endx, &log_endy);

	bulkInsertRaw(buf, startx, starty, data, datalen);

	adjustAllPoints(buf, startx, starty, log_endx, log_endy, 0);
}

/* Bulk-delete text from (startx, starty) to (endx, endy).
 * Uses direct memmove/memcpy and delRow — no character-at-a-time
 * primitives.  Does NOT record undo.  Calls adjustAllPoints. */
void bulkDelete(struct buffer *buf, int startx, int starty, int endx,
		int endy) {
	if (starty >= buf->numrows)
		return;

	/* Adjust tracked points before the mutation changes row structure */
	adjustAllPoints(buf, startx, starty, endx, endy, 1);

	if (starty == endy) {
		/* Single-row deletion */
		struct erow *row = &buf->row[starty];
		memmove(&row->chars[startx], &row->chars[endx],
			row->size - endx + 1); /* +1 for NUL */
		row->size -= endx - startx;
		row->cached_width = -1;
		row->cached_sublines = -1;
		markBufferDirty(buf);
		invalidateScreenCache(buf);
	} else {
		/* Multi-row deletion:
		 *   1. Delete interior rows (between starty and endy).
		 *   2. Merge start row prefix with end row suffix. */
		int rows_to_del = endy - starty - 1;
		for (int i = 0; i < rows_to_del; i++)
			delRow(buf, starty + 1);

		/* After deleting interior rows, the end row is now at
		 * starty + 1 */
		if (starty + 1 >= buf->numrows)
			return;

		struct erow *first = &buf->row[starty];
		struct erow *last = &buf->row[starty + 1];
		int new_size = startx + (last->size - endx);
		first->chars = xrealloc(first->chars, new_size + 1);
		first->charcap = new_size + 1;
		memcpy(&first->chars[startx], &last->chars[endx],
		       last->size - endx);
		first->size = new_size;
		first->chars[first->size] = '\0';
		first->cached_width = -1;
		first->cached_sublines = -1;
		delRow(buf, starty + 1);
		markBufferDirty(buf);
		invalidateScreenCache(buf);
	}
}

/* Apply one undo or redo step: replay the mutation and move the node
 * between the two lists.  Called only from doUndo/doRedo. */
static void undoStep(struct buffer *buf, int redo) {
	struct undo **src = redo ? &buf->redo : &buf->undo;
	struct undo **dst = redo ? &buf->undo : &buf->redo;
	struct undo *node = *src;
	int is_delete = redo ? node->delete : !node->delete;

	if (is_delete) {
		bulkDelete(buf, node->startx, node->starty, node->endx,
			   node->endy);
		buf->cx = node->startx;
		buf->cy = node->starty;
	} else {
		bulkInsert(buf, node->startx, node->starty, node->data,
			   node->datalen);
		buf->cx = node->endx;
		buf->cy = node->endy;
	}

	updateBuffer(buf);

	/* Move node from src-list head to dst-list head */
	struct undo *prev_dst = *dst;
	*dst = node;
	*src = node->prev;
	node->prev = prev_dst;

	/* Close whatever now sits at the head of the undo list.  A redo
	 * puts a record back that may still have been open when it was
	 * undone; without this, typing straight after a redo could fold
	 * into it and the run would no longer be one uninterrupted
	 * burst of editing. */
	undoCloseRun(buf);
}

void doUndo(struct buffer *buf, int count) {
	if (rejectIfReadOnly(buf))
		return;

	buf->mark_active = 0;
	undoCloseRun(buf);

	int times = UARG_COUNT(count);
	for (int j = 0; j < times; j++) {
		if (buf->undo == NULL) {
			setStatusMessage("No further undo information.");
			if (!buf->internal_mod) {
				markBufferClean(buf);
			}
			return;
		}
		int paired = buf->undo->paired;
		undoStep(buf, 0);
		setStatusMessage("Undo.");

		if (paired) {
			doUndo(buf, 1);
		}
	}
}

void doRedo(struct buffer *buf, int count) {
	if (rejectIfReadOnly(buf))
		return;

	buf->mark_active = 0;
	undoCloseRun(buf);

	int times = UARG_COUNT(count);
	for (int j = 0; j < times; j++) {
		if (buf->redo == NULL) {
			setStatusMessage("No further redo information.");
			return;
		}
		undoStep(buf, 1);
		setStatusMessage("Redo.");

		if (buf->redo != NULL && buf->redo->paired) {
			doRedo(buf, 1);
		}
	}
}

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
	ret->delete = 0;
	ret->datalen = 0;
	ret->datasize = 22;
	ret->data = xmalloc(ret->datasize);
	ret->data[0] = 0;
	return ret;
}

/* Replace an undo record's data buffer with a new allocation of
 * 'newsize' bytes.  Callers must fill in the new data themselves
 * after this returns. */
void undoReplaceData(struct undo *u, int newsize) {
	free(u->data);
	u->datasize = newsize;
	u->data = xmalloc(u->datasize);
}

#define ALIGNED(x1, y1, x2, y2) ((x1 == x2) && (y1 == y2))

/* Maximum number of operations folded into one record.  Counted as
 * operations, not bytes, so a run of CJK characters breaks at the
 * same place a run of ASCII does.
 *
 * Unbounded runs are correct but unrecoverable: one undo would throw
 * away an arbitrarily long burst of typing with no way to get part of
 * it back, because the intermediate states were never recorded.  A cap
 * that is too tight only costs the user extra keypresses. */
#define UNDO_MERGE_LIMIT 20

/* Grow u->data to hold at least 'needed' bytes plus a NUL. */
static void undoEnsureData(struct undo *u, int needed) {
	if (needed + 1 <= u->datasize)
		return;
	int newsize = u->datasize ? u->datasize : 22;
	while (newsize < needed + 1)
		newsize *= 2;
	u->data = xrealloc(u->data, newsize);
	u->datasize = newsize;
}

/* Try to fold 'new' into 'prev', which must be the head of the undo
 * list.  Returns 1 if merged ('new' is then spent and must be freed by
 * the caller), 0 if the records cannot be joined.
 *
 * Pure record arithmetic: no buffer, no cursor, no anchoring.  It
 * rests on the invariant every record in this design satisfies —
 *
 *     end == computeInsertEnd(data, start)
 *
 * which is what undoStep already relies on, since undoing a delete
 * replays it as bulkInsert(start, data) and redoing it as
 * bulkDelete(start..end).  Given that, the merged end never has to be
 * reasoned about: it is recomputed from the merged payload.
 *
 * Three shapes join, and nothing else does:
 *
 *   typing     prev.end   == new.start   append,  start unchanged
 *   C-d        prev.start == new.start   append,  start unchanged
 *   backspace  prev.start == new.end     prepend, start := new.start
 *
 * The backspace case is why the record start is taken from 'new':
 * deletion walks leftwards, so each operation extends the record's
 * reach backwards while its data must stay in forward file order. */
static int undoMerge(struct undo *prev, struct undo *new) {
	if (prev == NULL || new == NULL)
		return 0;
	if (!prev->append)
		return 0;
	if (prev->delete != new->delete)
		return 0;
	if (prev->paired || new->paired)
		return 0;
	if (prev->nmerges >= UNDO_MERGE_LIMIT)
		return 0;
	if (new->datalen <= 0)
		return 0;

	int prepend = 0;
	if (!new->delete) {
		if (!ALIGNED(prev->endx, prev->endy, new->startx, new->starty))
			return 0;
	} else if (ALIGNED(prev->startx, prev->starty, new->startx,
			   new->starty)) {
		/* forward delete: the buffer collapses to the start
		 * point, so successive deletions arrive at the same
		 * coordinates */
	} else if (ALIGNED(prev->startx, prev->starty, new->endx, new->endy)) {
		prepend = 1;
	} else {
		return 0;
	}

	undoEnsureData(prev, prev->datalen + new->datalen);
	if (prepend) {
		memmove(&prev->data[new->datalen], prev->data, prev->datalen);
		memcpy(prev->data, new->data, new->datalen);
		prev->startx = new->startx;
		prev->starty = new->starty;
	} else {
		memcpy(&prev->data[prev->datalen], new->data, new->datalen);
	}
	prev->datalen += new->datalen;
	prev->data[prev->datalen] = 0;
	prev->nmerges++;

	computeInsertEnd(prev->data, prev->datalen, prev->startx, prev->starty,
			 &prev->endx, &prev->endy);
	return 1;
}

static void freeUndos(struct undo *first);

/* Add a record to the undo list, taking ownership.
 *
 * A record arrives with 'append' already set by the mutation layer:
 * 1 means "this was a single interactive edit and may continue the run
 * at the head of the list", 0 means "this stands on its own".  Only
 * the mutation layer can tell a keystroke from a regex replace, so
 * only it makes that call; whether two given records can actually be
 * joined is decided here, as arithmetic on the records alone.
 *
 * A record that does not merge closes whatever run was open.  Without
 * that, typing either side of a bulk operation would find the earlier
 * record still aligned and fold across it. */
void pushUndo(struct buffer *buf, struct undo *new) {
	if (new->append && undoMerge(buf->undo, new)) {
		new->prev = NULL;
		freeUndos(new);
		return;
	}
	if (buf->undo != NULL)
		buf->undo->append = 0;
	new->prev = buf->undo;
	buf->undo = new;
}

/* Close any open run.  Called after undo or redo: typing straight
 * afterwards can land aligned with the record the operation exposed,
 * and while folding into it would still produce a correct record, "a
 * run is one uninterrupted burst of editing" is a cheaper invariant to
 * hold than to re-derive at each use. */
void undoCloseRun(struct buffer *buf) {
	if (buf->undo != NULL)
		buf->undo->append = 0;
}

static void freeUndos(struct undo *first) {
	struct undo *cur = first;
	struct undo *prev;

	while (cur != NULL) {
		free(cur->data);
		prev = cur;
		cur = prev->prev;
		free(prev);
	}
}

void clearRedos(struct buffer *buf) {
	freeUndos(buf->redo);
	buf->redo = NULL;
}

void clearUndosAndRedos(struct buffer *buf) {
	freeUndos(buf->undo);
	buf->undo = NULL;
	clearRedos(buf);
}
