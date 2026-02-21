#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "emil.h"
#include "message.h"
#include "region.h"
#include "buffer.h"
#include "undo.h"
#include "unicode.h"
#include "display.h"
#include "unused.h"
#include "util.h"

/* Bulk-insert text from 'data' (length 'datalen') into 'buf' starting
 * at buffer position (startx, starty).  Uses direct memmove/memcpy and
 * editorInsertRow — no character-at-a-time primitives.  Does NOT record
 * undo. */
static void bulkInsert(struct editorBuffer *buf, int startx, int starty,
		       const uint8_t *data, int datalen) {
	if (datalen <= 0)
		return;

	/* Ensure the target row exists */
	if (starty >= buf->numrows)
		editorInsertRow(buf, buf->numrows, "", 0);

	/* Scan for newlines to decide single-line vs multi-line */
	const uint8_t *first_nl = memchr(data, '\n', datalen);

	if (first_nl == NULL) {
		/* Single-line insert: memmove tail right, memcpy data in */
		struct erow *row = &buf->row[starty];
		row->chars = xrealloc(row->chars, row->size + datalen + 1);
		memmove(&row->chars[startx + datalen], &row->chars[startx],
			row->size - startx + 1); /* +1 for NUL */
		memcpy(&row->chars[startx], data, datalen);
		row->size += datalen;
		row->cached_width = -1;
		buf->dirty = 1;
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
			editorInsertRow(buf, insert_at, (char *)combined,
					combined_len);
			free(combined);
			free(suffix);
			buf->dirty = 1;
			invalidateScreenCache(buf);
			return;
		}
		/* Interior complete line */
		int line_len = (int)(nl - p);
		editorInsertRow(buf, insert_at, (char *)p, line_len);
		insert_at++;
		p = nl + 1;
	}

	/* If data ended with '\n', we still need to insert the suffix
	 * as a new row */
	if (suffix_len > 0) {
		editorInsertRow(buf, insert_at, (char *)suffix, suffix_len);
	} else {
		editorInsertRow(buf, insert_at, "", 0);
	}
	free(suffix);
	buf->dirty = 1;
	invalidateScreenCache(buf);
}

/* Bulk-delete text from (startx, starty) to (endx, endy).
 * Uses direct memmove/memcpy and editorDelRow — no character-at-a-time
 * primitives.  Does NOT record undo. */
static void bulkDelete(struct editorBuffer *buf, int startx, int starty,
		       int endx, int endy) {
	if (buf->numrows == 0 || starty >= buf->numrows)
		return;

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
			editorDelRow(buf, starty + 1);

		/* After deleting interior rows, the end row is now at
		 * starty + 1 */
		if (starty + 1 >= buf->numrows)
			return;

		struct erow *first = &buf->row[starty];
		struct erow *last = &buf->row[starty + 1];
		int new_size = startx + (last->size - endx);
		first->chars = xrealloc(first->chars, new_size + 1);
		memcpy(&first->chars[startx], &last->chars[endx],
		       last->size - endx);
		first->size = new_size;
		first->chars[first->size] = '\0';
		first->cached_width = -1;
		editorDelRow(buf, starty + 1);
		buf->dirty = 1;
		invalidateScreenCache(buf);
	}
}

void editorDoUndo(struct editorBuffer *buf, int count) {
	if (buf->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	int times = count ? count : 1;
	for (int j = 0; j < times; j++) {
		if (buf->undo == NULL) {
			editorSetStatusMessage("No further undo information.");
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

		editorUpdateBuffer(buf);

		struct editorUndo *orig = buf->redo;
		buf->redo = buf->undo;
		buf->undo = buf->undo->prev;
		buf->redo->prev = orig;
		buf->undo_count--;

		if (paired) {
			editorDoUndo(buf, 1);
		}
	}
}

#ifdef EMIL_DEBUG_UNDO
void debugUnpair(struct editorConfig *UNUSED(ed), struct editorBuffer *buf) {
	int undos = 0;
	int redos = 0;
	for (struct editorUndo *i = buf->undo; i; i = i->prev) {
		i->paired = 0;
		undos++;
	}
	for (struct editorUndo *i = buf->redo; i; i = i->prev) {
		i->paired = 0;
		redos++;
	}
	editorSetStatusMessage("Unpaired %d undos, %d redos.", undos, redos);
}
#endif

void editorDoRedo(struct editorBuffer *buf, int count) {
	if (buf->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	int times = count ? count : 1;
	for (int j = 0; j < times; j++) {
		if (buf->redo == NULL) {
			editorSetStatusMessage("No further redo information.");
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

		editorUpdateBuffer(buf);

		struct editorUndo *orig = buf->undo;
		buf->undo = buf->redo;
		buf->redo = buf->redo->prev;
		buf->undo->prev = orig;
		buf->undo_count++;

		if (buf->redo != NULL && buf->redo->paired) {
			editorDoRedo(buf, 1);
		}
	}
}

struct editorUndo *newUndo(void) {
	struct editorUndo *ret = xmalloc(sizeof(*ret));
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

static void freeUndos(struct editorUndo *first);

void pushUndo(struct editorBuffer *buf, struct editorUndo *new) {
	new->prev = buf->undo;
	buf->undo = new;
	buf->undo_count++;

	if (buf->undo_count > UNDO_LIMIT) {
		/* Walk to the node just before the tail to prune */
		struct editorUndo *cur = buf->undo;
		for (int i = 1; i < UNDO_LIMIT && cur->prev != NULL; i++) {
			cur = cur->prev;
		}
		if (cur->prev != NULL) {
			/* If the oldest entry is paired, free both */
			freeUndos(cur->prev);
			cur->prev = NULL;
		}
		buf->undo_count = UNDO_LIMIT;
	}
}

static void freeUndos(struct editorUndo *first) {
	struct editorUndo *cur = first;
	struct editorUndo *prev;

	while (cur != NULL) {
		free(cur->data);
		prev = cur;
		cur = prev->prev;
		free(prev);
	}
}

void clearRedos(struct editorBuffer *buf) {
	freeUndos(buf->redo);
	buf->redo = NULL;
}

void clearUndosAndRedos(struct editorBuffer *buf) {
	freeUndos(buf->undo);
	buf->undo = NULL;
	buf->undo_count = 0;
	clearRedos(buf);
}

#define ALIGNED(x1, y1, x2, y2) ((x1 == x2) && (y1 == y2))

void editorUndoAppendChar(struct editorBuffer *buf, uint8_t c) {
	clearRedos(buf);
	if (buf->undo == NULL || !(buf->undo->append) || buf->undo->delete ||
	    !ALIGNED(buf->undo->endx, buf->undo->endy, buf->cx, buf->cy)) {
		if (buf->undo != NULL)
			buf->undo->append = 0;
		struct editorUndo *new = newUndo();
		new->startx = buf->cx;
		new->starty = buf->cy;
		new->endx = buf->cx;
		new->endy = buf->cy;
		pushUndo(buf, new);
	}
	buf->undo->data[buf->undo->datalen++] = c;
	buf->undo->data[buf->undo->datalen] = 0;
	if (buf->undo->datalen >= buf->undo->datasize - 2) {
		if ((size_t)buf->undo->datasize > SIZE_MAX / 2) {
			die("buffer size overflow");
		}
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
}

void editorUndoAppendUnicode(struct editorConfig *ed,
			     struct editorBuffer *buf) {
	clearRedos(buf);
	if (buf->undo == NULL || !(buf->undo->append) ||
	    (buf->undo->datalen + ed->nunicode >= buf->undo->datasize) ||
	    buf->undo->delete ||
	    !ALIGNED(buf->undo->endx, buf->undo->endy, buf->cx, buf->cy)) {
		if (buf->undo != NULL)
			buf->undo->append = 0;
		struct editorUndo *new = newUndo();
		new->startx = buf->cx;
		new->starty = buf->cy;
		new->endx = buf->cx;
		new->endy = buf->cy;
		pushUndo(buf, new);
	}
	for (int i = 0; i < ed->nunicode; i++) {
		buf->undo->data[buf->undo->datalen++] = ed->unicode[i];
	}
	buf->undo->data[buf->undo->datalen] = 0;
	buf->undo->append = !(buf->undo->datalen >= buf->undo->datasize - 2);
	buf->undo->endx += ed->nunicode;
}

void editorUndoBackSpace(struct editorBuffer *buf, uint8_t c) {
	clearRedos(buf);
	if (buf->undo == NULL || !(buf->undo->append) || !(buf->undo->delete) ||
	    !((c == '\n' && buf->undo->startx == 0 &&
	       buf->undo->starty == buf->cy) ||
	      (buf->cx + 1 == buf->undo->startx &&
	       buf->cy == buf->undo->starty))) {
		if (buf->undo != NULL)
			buf->undo->append = 0;
		struct editorUndo *new = newUndo();
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
		if ((size_t)buf->undo->datasize > SIZE_MAX / 2) {
			die("buffer size overflow");
		}
		buf->undo->datasize *= 2;
		buf->undo->data =
			xrealloc(buf->undo->data, buf->undo->datasize);
	}
	memmove(&buf->undo->data[1], buf->undo->data, buf->undo->datalen);
	buf->undo->data[0] = c;
	buf->undo->datalen++;
	buf->undo->data[buf->undo->datalen] = 0;
	if (c == '\n') {
		buf->undo->starty--;
		buf->undo->startx = buf->row[buf->undo->starty].size;
	} else {
		buf->undo->startx--;
	}
}

void editorUndoDelChar(struct editorBuffer *buf, erow *row) {
	clearRedos(buf);
	if (buf->undo == NULL || !(buf->undo->append) || !(buf->undo->delete) ||
	    !(buf->undo->startx == buf->cx && buf->undo->starty == buf->cy)) {
		if (buf->undo != NULL)
			buf->undo->append = 0;
		struct editorUndo *new = newUndo();
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
			if ((size_t)buf->undo->datasize > SIZE_MAX / 2) {
				die("buffer size overflow");
			}
			buf->undo->datasize *= 2;
			buf->undo->data =
				xrealloc(buf->undo->data, buf->undo->datasize);
		}
		buf->undo->data[buf->undo->datalen++] = '\n';
		buf->undo->data[buf->undo->datalen] = 0;
		buf->undo->endy++;
		buf->undo->endx = 0;
	} else {
		int n = utf8_nBytes(row->chars[buf->cx]);
		if (buf->undo->datalen + n >= buf->undo->datasize - 2) {
			if ((size_t)buf->undo->datasize > SIZE_MAX / 2) {
				die("buffer size overflow");
			}
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
	}
}
