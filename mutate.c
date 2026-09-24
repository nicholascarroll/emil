/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include "mutate.h"
#include "adjust.h"
#include "buffer.h"
#include "dbuf.h"
#include "undo.h"
#include "util.h"
#include "wrap.h"
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
				   int endx, int endy, int old_len,
				   int repl_len) {
	if (old_len <= 0 || repl_len > 0)
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

	struct undo *fix = newUndo();
	fix->startx = atx;
	fix->starty = aty;
	computeInsertEnd((const uint8_t *)"\n", 1, atx, aty, &fix->endx,
			 &fix->endy);
	fix->delete = 0;
	fix->append = 0;
	fix->paired = 1;
	undoReplaceData(fix, 2);
	fix->data[0] = '\n';
	fix->data[1] = 0;
	fix->datalen = 1;
	pushUndo(buf, fix);

	bulkInsert(buf, atx, aty, (const uint8_t *)"\n", 1);
}

/* Shared body of every mutation.  'coalesce' merges the record into
 * the run at the head of the undo list; honoured only when exactly one
 * record is pushed.  During an input burst the run is uncapped. */
static void mutateReplaceEx(struct buffer *buf, int startx, int starty,
			    int endx, int endy, const uint8_t *old_text,
			    int old_len, const uint8_t *repl, int repl_len,
			    int chain_to_prev, int coalesce, int *out_endx,
			    int *out_endy) {
	/* Before clearRedos; out-params untouched on refusal. */
	if (rejectIfReadOnly(buf))
		return;

	/* As above. */
	if (wantsFinalNewline(buf) &&
	    deletesOnlyFinalNewline(buf, startx, starty, endx, endy, old_len,
				    repl_len))
		return;

	int is_replace = (old_len > 0 && repl_len > 0);
	if (is_replace || chain_to_prev)
		coalesce = 0;

	clearRedos(buf);

	/* With chain_to_prev, the first record pushed pairs to the
	 * previous mutation; in a replace the ins pairs to the del. */

	/* Delete undo record */
	if (old_len > 0) {
		struct undo *del = newUndo();
		del->startx = startx;
		del->starty = starty;
		del->endx = endx;
		del->endy = endy;
		del->delete = 1;
		del->append = coalesce;
		del->uncapped = coalesce && E.input_burst;
		del->paired = chain_to_prev ? 1 : 0;
		undoReplaceData(del, old_len + 1);
		memcpy(del->data, old_text, old_len);
		del->data[old_len] = 0;
		del->datalen = old_len;
		pushUndo(buf, del);
	}

	/* Perform deletion (bulkDelete calls adjustAllPoints internally) */
	if (old_len > 0)
		bulkDelete(buf, startx, starty, endx, endy);

	/* The logical insert end, as told to the caller. */
	int iex = startx, iey = starty;
	if (repl_len > 0)
		computeInsertEnd(repl, repl_len, startx, starty, &iex, &iey);

	/* Insert undo record */
	if (repl_len > 0) {
		struct dbuf adata = DBUF_INIT;
		dbuf_append(&adata, repl, repl_len);

		struct undo *ins = newUndo();
		ins->startx = startx;
		ins->starty = starty;
		computeInsertEnd(adata.buf, adata.len, startx, starty,
				 &ins->endx, &ins->endy);
		ins->delete = 0;
		ins->append = coalesce;
		ins->uncapped = coalesce && E.input_burst;
		ins->paired = is_replace ? 1 : (chain_to_prev ? 1 : 0);
		undoReplaceData(ins, adata.len + 1);
		memcpy(ins->data, adata.buf, adata.len);
		ins->data[adata.len] = 0;
		ins->datalen = adata.len;
		pushUndo(buf, ins);
		dbuf_free(&adata);

		/* bulkInsert calls adjustAllPoints internally */
		bulkInsert(buf, startx, starty, repl, repl_len);
	}

	restoreFinalNewline(buf);

	markBufferDirty(buf);

	if (out_endx)
		*out_endx = iex;
	if (out_endy)
		*out_endy = iey;
}

void mutateReplace(struct buffer *buf, int startx, int starty, int endx,
		   int endy, const uint8_t *old_text, int old_len,
		   const uint8_t *repl, int repl_len, int chain_to_prev,
		   int *out_endx, int *out_endy) {
	mutateReplaceEx(buf, startx, starty, endx, endy, old_text, old_len,
			repl, repl_len, chain_to_prev, 0, out_endx, out_endy);
}

void mutateInsertChar(struct buffer *buf, int startx, int starty,
		      const uint8_t *text, int len, int *out_endx,
		      int *out_endy) {
	mutateReplaceEx(buf, startx, starty, startx, starty, NULL, 0, text, len,
			0, 1, out_endx, out_endy);
}

void mutateDeleteChar(struct buffer *buf, int startx, int starty, int endx,
		      int endy, const uint8_t *old_text, int old_len) {
	mutateReplaceEx(buf, startx, starty, endx, endy, old_text, old_len,
			NULL, 0, 0, 1, NULL, NULL);
}

void mutateDelete(struct buffer *buf, int startx, int starty, int endx,
		  int endy, const uint8_t *old_text, int old_len) {
	mutateReplace(buf, startx, starty, endx, endy, old_text, old_len, NULL,
		      0, 0, NULL, NULL);
}

void mutateInsert(struct buffer *buf, int startx, int starty,
		  const uint8_t *text, int len, int *out_endx, int *out_endy) {
	mutateReplace(buf, startx, starty, startx, starty, NULL, 0, text, len,
		      0, out_endx, out_endy);
}

void mutateExtendRows(struct buffer *buf, int from_row, int n_rows) {
	if (rejectIfReadOnly(buf))
		return;

	if (n_rows <= 0)
		return;

	clearRedos(buf);

	/* Append n empty rows at end of buffer. */
	for (int i = 0; i < n_rows; i++)
		insertRow(buf, buf->numrows, (const uint8_t *)"", 0);

	/* A pure-insert undo record of the appended newlines, heading a
	 * chain that a following mutateReplace may pair onto. */
	struct undo *ext = newUndo();
	int n_newlines;
	if (from_row == 0) {
		/* A rowless buffer: anchor at the origin, n-1 newlines. */
		ext->starty = 0;
		ext->startx = 0;
		n_newlines = n_rows - 1;
	} else {
		ext->starty = from_row - 1;
		ext->startx = buf->row[ext->starty].size;
		n_newlines = n_rows;
	}
	ext->endx = 0;
	ext->endy = buf->numrows - 1;
	if (n_newlines + 1 > ext->datasize) {
		ext->datasize = n_newlines + 1;
		ext->data = xrealloc(ext->data, ext->datasize);
	}
	memset(ext->data, '\n', n_newlines);
	ext->data[n_newlines] = 0;
	ext->datalen = n_newlines;
	ext->append = 0;
	ext->delete = 0;
	ext->paired = 0;
	pushUndo(buf, ext);

	adjustAllPoints(buf, ext->startx, ext->starty, ext->endx, ext->endy, 0);

	markBufferDirty(buf);
}
