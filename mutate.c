/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include "mutate.h"
#include "adjust.h"
#include "buffer.h"
#include "dbuf.h"
#include "message.h"
#include "undo.h"
#include "util.h"
#include "wrap.h"
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

uint8_t *collectRegionText(struct buffer *buf, int startx, int starty, int endx,
			   int endy, int *out_len) {
	struct dbuf d = DBUF_INIT;
	int lx = startx;
	int ly = starty;

	while (!(ly == endy && lx == endx)) {
		/* Safety: stop if we've gone past the target row */
		if (ly > endy || ly >= buf->numrows)
			break;
		if (lx >= buf->row[ly].size) {
			dbuf_byte(&d, '\n');
			ly++;
			lx = 0;
		} else {
			dbuf_byte(&d, buf->row[ly].chars[lx]);
			lx++;
		}
	}
	return dbuf_detach(&d, out_len);
}

/* Final-newline invariant: a file buffer's last row is empty, unless
 * the buffer is empty.
 *
 *     bufferIsEmpty(buf) || row[numrows - 1].size == 0 */
static int wantsFinalNewline(struct buffer *buf) {
	return buf != E.minibuf;
}

/* Would this mutation delete the final newline and nothing else?
 * The repair below would restore it, so it is refused. */
static int deletesOnlyFinalNewline(struct buffer *buf, int startx, int starty,
				   int endx, int endy, int repl_len) {
	if ((startx == endx && starty == endy) || repl_len > 0)
		return 0;
	if (buf->numrows < 2)
		return 0;
	if (buf->row[buf->numrows - 1].size != 0)
		return 0;
	/* An empty row before it: a real edit, deleting a blank line. */
	if (buf->row[buf->numrows - 2].size == 0)
		return 0;
	return starty == buf->numrows - 2 &&
	       startx == buf->row[buf->numrows - 2].size &&
	       endy == buf->numrows - 1 && endx == 0;
}

/* Restore the invariant after a mutation that consumed the final
 * newline along with real text */
static void restoreFinalNewline(struct buffer *buf) {
	if (!wantsFinalNewline(buf) || bufferIsEmpty(buf))
		return;
	if (buf->row[buf->numrows - 1].size == 0)
		return;

	int atx = buf->row[buf->numrows - 1].size;
	int aty = buf->numrows - 1;

	const uint8_t nl = '\n';
	int endx, endy;
	computeInsertEnd(&nl, 1, atx, aty, &endx, &endy);
	struct undo *fix = undoRecordNew(atx, aty, endx, endy, 0, &nl, 1);
	fix->paired = 1;
	pushUndo(buf, fix);

	bulkInsert(buf, atx, aty, (const uint8_t *)"\n", 1);
}

/* What a caller's "move to the end of the edit" gets when nothing changed. */
static void cursorAsEnd(const struct buffer *buf, int *out_endx,
			int *out_endy) {
	if (out_endx)
		*out_endx = buf->cx;
	if (out_endy)
		*out_endy = buf->cy;
}

/* Shared body of every mutation.  'coalesce' merges the record into
 * the run at the head of the undo list; honoured only when exactly one
 * record is pushed.  During an input burst the run is uncapped. */
static enum mutateResult mutateReplaceEx(struct buffer *buf, int startx,
					 int starty, int endx, int endy,
					 const uint8_t *repl, int repl_len,
					 int chain_to_prev, int coalesce,
					 int *out_endx, int *out_endy) {
	/* Before clearRedos; out-params untouched on refusal. */
	if (rejectIfReadOnly(buf))
		return MUTATE_REFUSED;

	if (!bufferRangeValid(buf, startx, starty, endx, endy)) {
		setStatusMessage("Invalid edit range; nothing changed");
		cursorAsEnd(buf, out_endx, out_endy);
		return MUTATE_INVALID;
	}

	/* As above. */
	if (wantsFinalNewline(buf) &&
	    deletesOnlyFinalNewline(buf, startx, starty, endx, endy,
				    repl_len)) {
		cursorAsEnd(buf, out_endx, out_endy);
		return MUTATE_REFUSED;
	}

	if (repl_len < 0 || repl_len > INT_MAX - 1)
		abort();

	/* What the delete record restores: the buffer's own text, read
	 * before anything changes.  The common pure insert skips this. */
	int old_len = 0;
	uint8_t *old_text = NULL;
	if (startx != endx || starty != endy)
		old_text = collectRegionText(buf, startx, starty, endx, endy,
					     &old_len);

	int is_replace = (old_len > 0 && repl_len > 0);
	if (is_replace || chain_to_prev)
		coalesce = 0;

	/* The cursor before the change, for undo to return to.  Callers
	 * move the cursor after mutating, so it is still where the
	 * command started. */
	int pointx = buf->cx, pointy = buf->cy;

	clearRedos(buf);

	/* With chain_to_prev, the first record pushed pairs to the
	 * previous mutation; in a replace the ins pairs to the del. */

	/* Delete undo record */
	if (old_len > 0) {
		struct undo *del = undoRecordNew(startx, starty, endx, endy, 1,
						 old_text, old_len);
		del->append = coalesce;
		del->uncapped = coalesce && E.input_burst;
		del->paired = chain_to_prev ? 1 : 0;
		del->pointx = pointx;
		del->pointy = pointy;
		pushUndo(buf, del);
	}

	/* Perform deletion (bulkDelete calls adjustAllPoints internally) */
	if (old_len > 0)
		bulkDelete(buf, startx, starty, endx, endy);
	free(old_text);

	/* The logical insert end, as told to the caller. */
	int iex = startx, iey = starty;
	if (repl_len > 0)
		computeInsertEnd(repl, repl_len, startx, starty, &iex, &iey);

	/* Insert undo record */
	if (repl_len > 0) {
		struct undo *ins = undoRecordNew(startx, starty, iex, iey, 0,
						 repl, repl_len);
		ins->append = coalesce;
		ins->uncapped = coalesce && E.input_burst;
		ins->paired = is_replace ? 1 : (chain_to_prev ? 1 : 0);
		ins->pointx = pointx;
		ins->pointy = pointy;
		pushUndo(buf, ins);

		/* bulkInsert calls adjustAllPoints internally */
		bulkInsert(buf, startx, starty, repl, repl_len);
	}

	restoreFinalNewline(buf);

	markBufferDirty(buf);

	if (out_endx)
		*out_endx = iex;
	if (out_endy)
		*out_endy = iey;
	return MUTATE_DONE;
}

enum mutateResult mutateReplace(struct buffer *buf, int startx, int starty,
				int endx, int endy, const uint8_t *repl,
				int repl_len, int chain_to_prev, int *out_endx,
				int *out_endy) {
	return mutateReplaceEx(buf, startx, starty, endx, endy, repl, repl_len,
			       chain_to_prev, 0, out_endx, out_endy);
}

enum mutateResult mutateInsertChar(struct buffer *buf, int startx, int starty,
				   const uint8_t *text, int len, int *out_endx,
				   int *out_endy) {
	return mutateReplaceEx(buf, startx, starty, startx, starty, text, len,
			       0, 1, out_endx, out_endy);
}

enum mutateResult mutateDeleteChar(struct buffer *buf, int startx, int starty,
				   int endx, int endy) {
	return mutateReplaceEx(buf, startx, starty, endx, endy, NULL, 0, 0, 1,
			       NULL, NULL);
}

enum mutateResult mutateDelete(struct buffer *buf, int startx, int starty,
			       int endx, int endy) {
	return mutateReplace(buf, startx, starty, endx, endy, NULL, 0, 0, NULL,
			     NULL);
}

enum mutateResult mutateInsert(struct buffer *buf, int startx, int starty,
			       const uint8_t *text, int len, int *out_endx,
			       int *out_endy) {
	return mutateReplace(buf, startx, starty, startx, starty, text, len, 0,
			     out_endx, out_endy);
}

enum mutateResult mutateExtendRows(struct buffer *buf, int from_row,
				   int n_rows) {
	if (rejectIfReadOnly(buf))
		return MUTATE_REFUSED;

	if (n_rows <= 0)
		return MUTATE_DONE;

	if (from_row != buf->numrows) {
		setStatusMessage("Invalid edit range; nothing changed");
		return MUTATE_INVALID;
	}

	clearRedos(buf);

	/* Append n empty rows at end of buffer. */
	for (int i = 0; i < n_rows; i++)
		insertRow(buf, buf->numrows, (const uint8_t *)"", 0);

	/* A pure-insert undo record of the appended newlines, heading a
	 * chain that a following mutateReplace may pair onto. */
	int n_newlines = from_row == 0 ? n_rows - 1 : n_rows;
	int anchory = from_row == 0 ? 0 : from_row - 1;
	/* A rowless buffer anchors at the origin, with one newline fewer. */
	int anchorx = from_row == 0 ? 0 : buf->row[anchory].size;
	struct undo *ext = undoRecordNew(anchorx, anchory, 0, buf->numrows - 1,
					 0, NULL, 0);
	undoRecordReserve(ext, n_newlines);
	memset(ext->data, '\n', n_newlines);
	ext->data[n_newlines] = 0;
	ext->datalen = n_newlines;
	pushUndo(buf, ext);

	adjustAllPoints(buf, ext->startx, ext->starty, ext->endx, ext->endy, 0);

	markBufferDirty(buf);
	return MUTATE_DONE;
}

static uint8_t *wholeText(struct buffer *buf, int *len) {
	int last = buf->numrows - 1;
	return collectRegionText(buf, 0, 0, buf->row[last].size, last, len);
}

struct mutateRebuild mutateRebuildBegin(struct buffer *buf) {
	struct mutateRebuild r = { NULL, 0 };
	if (buf->undo != NULL || buf->redo != NULL)
		r.old_text = wholeText(buf, &r.old_len);
	bufferResetRows(buf);
	return r;
}

void mutateRebuildEnd(struct buffer *buf, struct mutateRebuild *r) {
	if (r->old_text == NULL)
		return;

	bufferEnsureRow(buf);
	int new_len;
	uint8_t *new_text = wholeText(buf, &new_len);

	/* Put the old rows back, one per newline, then replace them. */
	bufferResetRows(buf);
	int start = 0, row = 0;
	for (int i = 0; i <= r->old_len; i++) {
		if (i == r->old_len || r->old_text[i] == '\n') {
			insertRow(buf, row++, &r->old_text[start], i - start);
			start = i + 1;
		}
	}

	int read_only = buf->read_only;
	int dirty = buf->dirty;
	int last = buf->numrows - 1;
	buf->read_only = 0;
	mutateReplace(buf, 0, 0, buf->row[last].size, last, new_text, new_len,
		      0, NULL, NULL);
	buf->read_only = read_only;
	buf->dirty = dirty;

	free(new_text);
	free(r->old_text);
	r->old_text = NULL;
}
