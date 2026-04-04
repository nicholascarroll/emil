#include "undo.h"
#include "adjust.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "message.h"
#include "region.h"
#include "unicode.h"
#include "unused.h"
#include "util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

extern struct config E;
/* Bulk-insert text from 'data' (length 'datalen') into 'buf' starting
 * at buffer position (startx, starty).  Uses direct memmove/memcpy and
 * insertRow — no character-at-a-time primitives.  Does NOT record
 * undo.  Calls adjustAllPoints internally. */
void bulkInsert(struct buffer *buf, int startx, int starty, const uint8_t *data,
		int datalen) {
	if (datalen <= 0)
		return;

	/* Ensure the target row exists */
	if (starty >= buf->numrows)
		insertRow(buf, buf->numrows, "", 0);

	/* Scan for newlines to decide single-line vs multi-line */
	const uint8_t *first_nl = memchr(data, '\n', datalen);

	if (first_nl == NULL) {
		/* Single-line insert: memmove tail right, memcpy data in */
		struct erow *row = &buf->row[starty];
		int needed = row->size + datalen + 1;
		if (needed > row->charcap) {
			int new_cap = row->charcap < 16 ? 16 : row->charcap * 2;
			if (new_cap < needed)
				new_cap = needed;
			row->chars = xrealloc(row->chars, new_cap);
			row->charcap = new_cap;
		}
		memmove(&row->chars[startx + datalen], &row->chars[startx],
			row->size - startx + 1); /* +1 for NUL */
		memcpy(&row->chars[startx], data, datalen);
		row->size += datalen;
		row->cached_width = -1;
		buf->dirty = 1;
		invalidateScreenCache(buf);
		adjustAllPoints(buf, startx, starty, startx + datalen, starty,
				0);
		return;
	}

	/* Multi-line insert.  Strategy:
	 *   1. Save the suffix of the start row (bytes after startx).
	 *   2. Truncate the start row at startx.
	 *   3. Append the first line fragment from data to the start row.
	 *   4. Insert complete interior lines as new rows.
	 *   5. Insert the last line fragment + saved suffix as a new row. */

	/* Pre-compute the end position of this insert for point adjustment.
	 * Walk the data to count newlines and find the last line length. */
	int ins_endx = startx;
	int ins_endy = starty;
	for (int i = 0; i < datalen; i++) {
		if (data[i] == '\n') {
			ins_endy++;
			ins_endx = 0;
		} else {
			ins_endx++;
		}
	}

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
			insertRow(buf, insert_at, (char *)combined,
				  combined_len);
			free(combined);
			free(suffix);
			buf->dirty = 1;
			invalidateScreenCache(buf);
			adjustAllPoints(buf, startx, starty, ins_endx, ins_endy,
					0);
			return;
		}
		/* Interior complete line */
		int line_len = (int)(nl - p);
		insertRow(buf, insert_at, (char *)p, line_len);
		insert_at++;
		p = nl + 1;
	}

	/* If data ended with '\n', we still need to insert the suffix
	 * as a new row */
	if (suffix_len > 0) {
		insertRow(buf, insert_at, (char *)suffix, suffix_len);
	} else {
		insertRow(buf, insert_at, "", 0);
	}
	free(suffix);
	buf->dirty = 1;
	invalidateScreenCache(buf);
	adjustAllPoints(buf, startx, starty, ins_endx, ins_endy, 0);
}

/* Bulk-delete text from (startx, starty) to (endx, endy).
 * Uses direct memmove/memcpy and delRow — no character-at-a-time
 * primitives.  Does NOT record undo. */
static void bulkDelete(struct buffer *buf, int startx, int starty, int endx,
		       int endy) {
	if (buf->numrows == 0 || starty >= buf->numrows)
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
		buf->dirty = 1;
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
		delRow(buf, starty + 1);
		buf->dirty = 1;
		invalidateScreenCache(buf);
	}
}

void doUndo(struct buffer *buf, int count) {
	if (buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	buf->mark_active = 0;

	int times = count ? count : 1;
	for (int j = 0; j < times; j++) {
		if (buf->undo == NULL) {
			setStatusMessage(msg_no_undo);
			if (!buf->undo_pruned && !buf->internal_mod) {
				buf->dirty = 0;
			}
			return;
		}
		int paired = buf->undo->paired;

		if (buf->undo->delete) {
			/* Re-insert deleted text using bulk operations.
			 * Data is in forward order (matching original
			 * file text). */
			bulkInsert(buf, buf->undo->startx, buf->undo->starty,
				   buf->undo->data, buf->undo->datalen);
			buf->cx = buf->undo->endx;
			buf->cy = buf->undo->endy;
		} else {
			/* Delete the previously inserted text using
			 * bulk operations. */
			bulkDelete(buf, buf->undo->startx, buf->undo->starty,
				   buf->undo->endx, buf->undo->endy);
			buf->cx = buf->undo->startx;
			buf->cy = buf->undo->starty;
		}

		updateBuffer(buf);

		struct undo *orig = buf->redo;
		buf->redo = buf->undo;
		buf->undo = buf->undo->prev;
		buf->redo->prev = orig;
		buf->undo_count--;
		setStatusMessage(msg_undo);

		if (paired) {
			doUndo(buf, 1);
		}
	}
}

#ifdef EMIL_DEBUG_UNDO
void debugUnpair(void) {
	struct buffer *buf = E.buf;
	int undos = 0;
	int redos = 0;
	for (struct undo *i = buf->undo; i; i = i->prev) {
		i->paired = 0;
		undos++;
	}
	for (struct undo *i = buf->redo; i; i = i->prev) {
		i->paired = 0;
		redos++;
	}
	setStatusMessage(msg_unpaired_undo_redo, undos, redos);
}
#endif

void doRedo(struct buffer *buf, int count) {
	if (buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	buf->mark_active = 0;

	int times = count ? count : 1;
	for (int j = 0; j < times; j++) {
		if (buf->redo == NULL) {
			setStatusMessage(msg_no_redo);
			return;
		}

		if (buf->redo->delete) {
			/* Re-delete text using bulk operations. */
			bulkDelete(buf, buf->redo->startx, buf->redo->starty,
				   buf->redo->endx, buf->redo->endy);
			buf->cx = buf->redo->startx;
			buf->cy = buf->redo->starty;
		} else {
			/* Re-insert text using bulk operations.
			 * Data is in forward order. */
			bulkInsert(buf, buf->redo->startx, buf->redo->starty,
				   buf->redo->data, buf->redo->datalen);
			buf->cx = buf->redo->endx;
			buf->cy = buf->redo->endy;
		}

		updateBuffer(buf);

		struct undo *orig = buf->undo;
		buf->undo = buf->redo;
		buf->redo = buf->redo->prev;
		buf->undo->prev = orig;
		buf->undo_count++;

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
	ret->append = 1;
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

static void freeUndos(struct undo *first);

void pushUndo(struct buffer *buf, struct undo *new) {
	new->prev = buf->undo;
	buf->undo = new;
	buf->undo_count++;

	if (buf->undo_count > UNDO_LIMIT) {
		/* Walk to the node just before the tail to prune */
		struct undo *cur = buf->undo;
		for (int i = 1; i < UNDO_LIMIT && cur->prev != NULL; i++) {
			cur = cur->prev;
		}
		if (cur->prev != NULL) {
			/* If the oldest entry is paired, free both */
			freeUndos(cur->prev);
			cur->prev = NULL;
			cur->paired = 0; /* pair was split by pruning */
		}
		buf->undo_count = UNDO_LIMIT;
		buf->undo_pruned = 1;
	}
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
	buf->undo_count = 0;
	clearRedos(buf);
}

#define ALIGNED(x1, y1, x2, y2) ((x1 == x2) && (y1 == y2))

void undoAppendChar(struct buffer *buf, uint8_t c) {
	clearRedos(buf);
	if (buf->undo == NULL || !(buf->undo->append) || buf->undo->delete ||
	    !ALIGNED(buf->undo->endx, buf->undo->endy, buf->cx, buf->cy)) {
		if (buf->undo != NULL)
			buf->undo->append = 0;
		struct undo *new = newUndo();
		new->startx = buf->cx;
		new->starty = buf->cy;
		new->endx = buf->cx;
		new->endy = buf->cy;
		pushUndo(buf, new);
	}
	buf->undo->data[buf->undo->datalen++] = c;
	buf->undo->data[buf->undo->datalen] = 0;
	if (buf->undo->datalen >= buf->undo->datasize - 2) {
		buf->undo->datasize *= 2;
		buf->undo->data =
			xrealloc(buf->undo->data, buf->undo->datasize);
	}
	buf->undo->append = !(buf->undo->datalen >= buf->undo->datasize - 2);
	if (c == '\n') {
		buf->undo->endx = 0;
		buf->undo->endy++;
	} else {
		buf->undo->endx++;
	}

	/* Adjust tracked points for this single-char insertion.
	 * The char is inserted at (cx, cy) — which is the old endx/endy
	 * before the increment above.  After insertion, the new end is
	 * (endx, endy).  But the *insertion point* is the old end, which
	 * equals (cx, cy) since ALIGNED was checked above. */
	{
		int sx = buf->cx;
		int sy = buf->cy;
		int ex, ey;
		if (c == '\n') {
			ex = 0;
			ey = sy + 1;
		} else {
			ex = sx + 1;
			ey = sy;
		}
		adjustAllPoints(buf, sx, sy, ex, ey, 0);
	}
}

void undoAppendUnicode(struct buffer *buf) {
	clearRedos(buf);
	if (buf->undo == NULL || !(buf->undo->append) ||
	    (buf->undo->datalen + E.nunicode >= buf->undo->datasize) ||
	    buf->undo->delete ||
	    !ALIGNED(buf->undo->endx, buf->undo->endy, buf->cx, buf->cy)) {
		if (buf->undo != NULL)
			buf->undo->append = 0;
		struct undo *new = newUndo();
		new->startx = buf->cx;
		new->starty = buf->cy;
		new->endx = buf->cx;
		new->endy = buf->cy;
		pushUndo(buf, new);
	}
	for (int i = 0; i < E.nunicode; i++) {
		buf->undo->data[buf->undo->datalen++] = E.unicode[i];
	}
	buf->undo->data[buf->undo->datalen] = 0;
	buf->undo->append = !(buf->undo->datalen >= buf->undo->datasize - 2);
	buf->undo->endx += E.nunicode;

	/* Adjust tracked points for this unicode insertion (always same-line) */
	adjustAllPoints(buf, buf->cx, buf->cy, buf->cx + E.nunicode, buf->cy,
			0);
}

void undoBackSpace(struct buffer *buf, uint8_t c) {
	clearRedos(buf);
	if (buf->undo == NULL || !(buf->undo->append) || !(buf->undo->delete) ||
	    !((c == '\n' && buf->undo->startx == 0 &&
	       buf->undo->starty == buf->cy) ||
	      (buf->cx + 1 == buf->undo->startx &&
	       buf->cy == buf->undo->starty))) {
		if (buf->undo != NULL)
			buf->undo->append = 0;
		struct undo *new = newUndo();
		new->endx = buf->cx;
		if (c != '\n')
			new->endx++;
		new->endy = buf->cy;
		new->startx = new->endx;
		new->starty = buf->cy;
		new->delete = 1;
		pushUndo(buf, new);
	}
	/* Prepend the byte so data stays in forward (file) order.
	 * Backspace delivers bytes from right to left, so prepending
	 * reconstructs the original left-to-right sequence. */
	if (buf->undo->datalen + 1 >= buf->undo->datasize - 2) {
		buf->undo->datasize *= 2;
		buf->undo->data =
			xrealloc(buf->undo->data, buf->undo->datasize);
	}
	memmove(&buf->undo->data[1], buf->undo->data, buf->undo->datalen);
	buf->undo->data[0] = c;
	buf->undo->datalen++;
	buf->undo->data[buf->undo->datalen] = 0;

	/* Capture old start before adjusting the undo range */
	int old_startx = buf->undo->startx;
	int old_starty = buf->undo->starty;

	if (c == '\n') {
		buf->undo->starty--;
		buf->undo->startx = buf->row[buf->undo->starty].size;
	} else {
		buf->undo->startx--;
	}

	/* Adjust tracked points for this single-char deletion.
	 * The deleted range is from the new start to the old start. */
	adjustAllPoints(buf, buf->undo->startx, buf->undo->starty, old_startx,
			old_starty, 1);
}

void undoDelChar(struct buffer *buf, erow *row) {
	clearRedos(buf);
	if (buf->undo == NULL || !(buf->undo->append) || !(buf->undo->delete) ||
	    !(buf->undo->startx == buf->cx && buf->undo->starty == buf->cy)) {
		if (buf->undo != NULL)
			buf->undo->append = 0;
		struct undo *new = newUndo();
		new->endx = buf->cx;
		new->endy = buf->cy;
		new->startx = buf->cx;
		new->starty = buf->cy;
		new->delete = 1;
		pushUndo(buf, new);
	}

	if (buf->cx == row->size) {
		/* Deleting a newline — append it */
		if (buf->undo->datalen >= buf->undo->datasize - 2) {
			buf->undo->datasize *= 2;
			buf->undo->data =
				xrealloc(buf->undo->data, buf->undo->datasize);
		}
		buf->undo->data[buf->undo->datalen++] = '\n';
		buf->undo->data[buf->undo->datalen] = 0;
		buf->undo->endy++;
		buf->undo->endx = 0;

		/* Deleting newline: merges (cx, cy) with (0, cy+1) */
		adjustAllPoints(buf, buf->cx, buf->cy, 0, buf->cy + 1, 1);
	} else {
		int n = utf8_nBytes(row->chars[buf->cx]);
		if (buf->undo->datalen + n >= buf->undo->datasize - 2) {
			buf->undo->datasize *= 2;
			if (buf->undo->datalen + n >= buf->undo->datasize - 2) {
				buf->undo->datasize =
					buf->undo->datalen + n + 4;
			}
			buf->undo->data =
				xrealloc(buf->undo->data, buf->undo->datasize);
		}
		/* Append bytes in natural UTF-8 order */
		for (int i = 0; i < n; i++) {
			buf->undo->data[buf->undo->datalen++] =
				row->chars[buf->cx + i];
		}
		buf->undo->data[buf->undo->datalen] = 0;
		buf->undo->endx += n;

		/* Deleting n bytes on same line at cursor */
		adjustAllPoints(buf, buf->cx, buf->cy, buf->cx + n, buf->cy, 1);
	}
}

void undoSelfInsert(uint8_t c, int count) {
	if (count == 1) {
		undoAppendChar(E.buf, c);
		return;
	}
	clearRedos(E.buf);
	struct undo *new = newUndo();
	new->startx = E.buf->cx;
	new->starty = E.buf->cy;
	new->endx = E.buf->cx + count;
	new->endy = E.buf->cy;
	new->append = 0;
	if (count + 1 > new->datasize) {
		new->datasize = count + 1;
		new->data = xrealloc(new->data, new->datasize);
	}
	memset(new->data, c, count);
	new->data[count] = 0;
	new->datalen = count;
	pushUndo(E.buf, new);
}
