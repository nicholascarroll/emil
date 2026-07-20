#include "mutate.h"
#include "adjust.h"
#include "buffer.h"
#include "dbuf.h"
#include "undo.h"
#include "util.h"
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

/* Compute the end position after inserting 'len' bytes of 'text'
 * starting at (startx, starty). */
static void computeInsertEnd(const uint8_t *text, int len, int startx,
			     int starty, int *endx, int *endy) {
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

void mutateReplace(struct buffer *buf, int startx, int starty, int endx,
		   int endy, const uint8_t *old_text, int old_len,
		   const uint8_t *repl, int repl_len, int chain_to_prev,
		   int *out_endx, int *out_endy) {
	/* Authoritative read-only check for the mutation layer.  Must
	 * precede clearRedos; on refusal the out-params are untouched
	 * (see mutate.h). */
	if (rejectIfReadOnly(buf))
		return;

	int is_replace = (old_len > 0 && repl_len > 0);

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
		del->append = 0;
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

	/* Compute insert end position */
	int iex = startx, iey = starty;
	if (repl_len > 0)
		computeInsertEnd(repl, repl_len, startx, starty, &iex, &iey);

	/* Insert undo record.  paired=1 if this pairs to the del just
	 * above (is_replace), or if chain_to_prev is set and the del was
	 * empty — in the latter case this ins is the "first record" and
	 * takes the chain. */
	if (repl_len > 0) {
		struct undo *ins = newUndo();
		ins->startx = startx;
		ins->starty = starty;
		ins->endx = iex;
		ins->endy = iey;
		ins->delete = 0;
		ins->append = 0;
		ins->paired = is_replace ? 1 : (chain_to_prev ? 1 : 0);
		undoReplaceData(ins, repl_len + 1);
		memcpy(ins->data, repl, repl_len);
		ins->data[repl_len] = 0;
		ins->datalen = repl_len;
		pushUndo(buf, ins);

		/* bulkInsert calls adjustAllPoints internally */
		bulkInsert(buf, startx, starty, repl, repl_len);
	}

	markBufferDirty(buf);
	updateBuffer(buf);

	if (out_endx)
		*out_endx = iex;
	if (out_endy)
		*out_endy = iey;
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

	/* Build a pure-insert undo record matching the shape yankRectangle
	 * hand-built before this helper existed:
	 *   starty = last row of original buffer
	 *   startx = end of that row (i.e. where the first newline was
	 *            appended)
	 *   endy   = last row of extended buffer
	 *   endx   = 0 (cursor sits at start of the last empty row)
	 *   data   = n_rows '\n' bytes
	 *
	 * paired=0 — this is the head of a chain; a following
	 * mutateReplace with chain_to_prev=1 pairs onto it. */
	struct undo *ext = newUndo();
	int n_newlines;
	if (from_row == 0) {
		/* Extending a rowless buffer: there is no preceding row
		 * to anchor to, so the record starts at the origin and
		 * the n inserted rows read as n-1 joining newlines.
		 * Undoing restores a single empty row — the closest
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
	updateBuffer(buf);
}
