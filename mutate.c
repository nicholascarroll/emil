#include "mutate.h"
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
		   const uint8_t *repl, int repl_len, int *out_endx,
		   int *out_endy) {
	int is_replace = (old_len > 0 && repl_len > 0);

	clearRedos(buf);

	/* Delete undo record */
	if (old_len > 0) {
		struct undo *del = newUndo();
		del->startx = startx;
		del->starty = starty;
		del->endx = endx;
		del->endy = endy;
		del->delete = 1;
		del->append = 0;
		del->paired = 0;
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

	/* Insert undo record */
	if (repl_len > 0) {
		struct undo *ins = newUndo();
		ins->startx = startx;
		ins->starty = starty;
		ins->endx = iex;
		ins->endy = iey;
		ins->delete = 0;
		ins->append = 0;
		ins->paired = is_replace ? 1 : 0;
		undoReplaceData(ins, repl_len + 1);
		memcpy(ins->data, repl, repl_len);
		ins->data[repl_len] = 0;
		ins->datalen = repl_len;
		pushUndo(buf, ins);

		/* bulkInsert calls adjustAllPoints internally */
		bulkInsert(buf, startx, starty, repl, repl_len);
	}

	buf->dirty = 1;
	updateBuffer(buf);

	if (out_endx)
		*out_endx = iex;
	if (out_endy)
		*out_endy = iey;
}

void mutateDelete(struct buffer *buf, int startx, int starty, int endx,
		  int endy, const uint8_t *old_text, int old_len) {
	mutateReplace(buf, startx, starty, endx, endy, old_text, old_len, NULL,
		      0, NULL, NULL);
}

void mutateInsert(struct buffer *buf, int startx, int starty,
		  const uint8_t *text, int len, int *out_endx, int *out_endy) {
	mutateReplace(buf, startx, starty, startx, starty, NULL, 0, text, len,
		      out_endx, out_endy);
}
