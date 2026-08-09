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

/* ---- Final-newline invariant ----
 *
 * A file buffer ends in a newline, which under the representation
 * means its last row is empty -- unless the buffer is empty, which has
 * no lines to terminate and must still serialise to zero bytes.
 *
 *     bufferIsEmpty(buf) || row[numrows - 1].size == 0
 *
 * newBuffer establishes it (the empty buffer satisfies it), editorOpen
 * establishes it by appending the row a file without a trailing
 * newline lacks, and the two functions below maintain it.  save()
 * therefore has no policy of its own: it serialises a buffer that is
 * already correct. */
static int wantsFinalNewline(struct buffer *buf) {
	return buf != E.minibuf;
}

/* Would this mutation delete the final newline and nothing else?
 *
 * Such a request is a pure no-op under the final-newline invariant.
 * The deletion would be immediately restored by the repair below,
 * leaving the buffer byte-identical while adding two undo records,
 * marking the buffer dirty, and clearing the redo stack.  Refusing
 * the request up front preserves both the invariant and the integrity
 * of the undo history. */
static int deletesOnlyFinalNewline(struct buffer *buf, int startx, int starty,
				   int endx, int endy, int old_len,
				   int repl_len) {
	if (old_len <= 0 || repl_len > 0)
		return 0;
	if (buf->numrows < 2)
		return 0;
	if (buf->row[buf->numrows - 1].size != 0)
		return 0;
	/* Only a no-op if the repair would fire, which needs the row
	 * surviving the join to be non-empty.  An empty one leaves the
	 * invariant holding: this is a real edit, deleting a blank
	 * line. */
	if (buf->row[buf->numrows - 2].size == 0)
		return 0;
	return starty == buf->numrows - 2 &&
	       startx == buf->row[buf->numrows - 2].size &&
	       endy == buf->numrows - 1 && endx == 0;
}

/* Restore the invariant after a mutation that consumed the final
 * newline along with real text -- a kill-region running off the end, a
 * regex replace matching it.  The appended row is recorded with
 * paired=1, so it and the mutation that provoked it undo as one step;
 * this is the same mechanism a replace already uses to bind its delete
 * and insert together.  Nothing else is needed: a row appended past
 * the end of the buffer moves no tracked point. */
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

/* Shared body of every mutation.  'coalesce' asks that the single
 * record produced be offered to the run at the head of the undo list
 * rather than pushed on top of it; it is honoured only for a mutation
 * that pushes exactly one record, since a paired delete+insert is one
 * atomic unit and must not be half-absorbed into a typing run. */
static void mutateReplaceEx(struct buffer *buf, int startx, int starty,
			    int endx, int endy, const uint8_t *old_text,
			    int old_len, const uint8_t *repl, int repl_len,
			    int chain_to_prev, int coalesce, int *out_endx,
			    int *out_endy) {
	/* Authoritative read-only check for the mutation layer.  Must
	 * precede clearRedos; on refusal the out-params are untouched
	 * (see mutate.h). */
	if (rejectIfReadOnly(buf))
		return;

	/* Refused for the same reason and on the same terms as the
	 * read-only check above: before clearRedos, with the out-params
	 * left untouched (see mutate.h). */
	if (wantsFinalNewline(buf) &&
	    deletesOnlyFinalNewline(buf, startx, starty, endx, endy, old_len,
				    repl_len))
		return;

	int is_replace = (old_len > 0 && repl_len > 0);
	if (is_replace || chain_to_prev)
		coalesce = 0;

	clearRedos(buf);

	/* The first record pushed by this call pairs to the previous
	 * mutation if chain_to_prev is set.  When this is a replace
	 * (del + ins), the del record is first and takes the chain; the
	 * ins record always pairs to the del.  When only one side is
	 * non-empty, whichever is present is "first". */

	/* Delete undo record */
	if (old_len > 0) {
		struct undo *del = newUndo();
		del->startx = startx;
		del->starty = starty;
		del->endx = endx;
		del->endy = endy;
		del->delete = 1;
		del->append = coalesce;
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

	/* Compute insert end position.  This is the LOGICAL end: it is
	 * what the caller is told and what point adjustment uses.  The
	 * record below states the anchored end instead. */
	int iex = startx, iey = starty;
	if (repl_len > 0)
		computeInsertEnd(repl, repl_len, startx, starty, &iex, &iey);

	/* Insert undo record.  paired=1 if this pairs to the del just
	 * above (is_replace), or if chain_to_prev is set and the del was
	 * empty: in the latter case this ins is the "first record" and
	 * takes the chain. */
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
		ins->paired = is_replace ? 1 : (chain_to_prev ? 1 : 0);
		undoReplaceData(ins, adata.len + 1);
		memcpy(ins->data, adata.buf, adata.len);
		ins->data[adata.len] = 0;
		ins->datalen = adata.len;
		pushUndo(buf, ins);
		dbuf_free(&adata);

		/* bulkInsert anchors the row mutation itself and calls
		 * adjustAllPoints on the logical range. */
		bulkInsert(buf, startx, starty, repl, repl_len);
	}

	restoreFinalNewline(buf);

	markBufferDirty(buf);
	invalidateScreenCache(buf);

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

	/* Build a pure-insert undo record:
	 *   starty = last row of original buffer
	 *   startx = end of that row (i.e. where the first newline was
	 *            appended)
	 *   endy   = last row of extended buffer
	 *   endx   = 0 (cursor sits at start of the last empty row)
	 *   data   = n_rows '\n' bytes
	 *
	 * paired=0: this is the head of a chain; a following
	 * mutateReplace with chain_to_prev=1 pairs onto it. */
	struct undo *ext = newUndo();
	int n_newlines;
	if (from_row == 0) {
		/* Extending a rowless buffer: there is no preceding row
		 * to anchor to, so the record starts at the origin and
		 * the n inserted rows read as n-1 joining newlines.
		 * Undoing restores a single empty row: the closest
		 * representable state to a rowless buffer.  Without
		 * this case starty would be -1 and the buf->row[]
		 * read below is out of bounds. */
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
	invalidateScreenCache(buf);
}
