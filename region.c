#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <regex.h>
#include "emil.h"
#include "message.h"
#include "region.h"
#include "buffer.h"
#include "undo.h"
#include "display.h"
#include "history.h"
#include "prompt.h"
#include "util.h"
#include "adjust.h"

extern struct editorConfig E;

void addToKillRing(const char *text, int is_rect, int rect_width,
		   int rect_height) {
	if (!text || strlen(text) == 0)
		return;

	addHistoryWithRect(&E.kill_history, text, is_rect, rect_width,
			   rect_height);
	E.kill_ring_pos = -1;

	/* text may point to E.kill.str, so copy before freeing. */
	uint8_t *copy = (uint8_t *)xstrdup(text);
	clearEditorText(&E.kill);
	E.kill.str = copy;
	E.kill.is_rectangle = is_rect;
	E.kill.rect_width = rect_width;
	E.kill.rect_height = rect_height;
}

/* Save and restore the kill text around operations that temporarily
 * overwrite ed->kill (transforms, rectangle ops). */
static uint8_t *saveKill(struct editorConfig *ed) {
	if (ed->kill.str == NULL)
		return NULL;
	return (uint8_t *)xstrdup((char *)ed->kill.str);
}

static void restoreKill(struct editorConfig *ed, uint8_t *saved) {
	free(ed->kill.str);
	ed->kill.str = saved;
}

/* Push the current mark position onto the mark ring (if mark is valid). */
static void markRingPush(struct editorBuffer *buf) {
	if (buf->markx < 0 || buf->marky < 0)
		return;
	buf->mark_ring[buf->mark_ring_idx].cx = buf->markx;
	buf->mark_ring[buf->mark_ring_idx].cy = buf->marky;
	buf->mark_ring_idx = (buf->mark_ring_idx + 1) % MARK_RING_SIZE;
	if (buf->mark_ring_len < MARK_RING_SIZE)
		buf->mark_ring_len++;
}

void editorSetMark(void) {
	E.buf->rectangle_mode = 0;
	/* C-SPC C-SPC: if mark is already active at point, deactivate it.
	 * This lets you drop a mark for later pop-back without starting
	 * a visible selection. */
	if (E.buf->mark_active && E.buf->markx == E.buf->cx &&
	    E.buf->marky == E.buf->cy) {
		E.buf->mark_active = 0;
		editorSetStatusMessage(msg_mark_cleared);
		return;
	}
	markRingPush(E.buf);
	E.buf->markx = E.buf->cx;
	E.buf->marky = E.buf->cy;
	E.buf->mark_active = 1;
	editorSetStatusMessage(msg_mark_set);
	if (E.buf->marky >= E.buf->numrows) {
		E.buf->marky = E.buf->numrows - 1;
		E.buf->markx = E.buf->row[E.buf->marky].size;
	}
}

/* Set mark at point, push old mark onto ring, but do NOT activate
 * (no highlighting) and do NOT print a message.  Used before jumps
 * like isearch, M-<, M->, goto-line, register-jump so the user
 * can pop back with C-u C-SPC. */
void editorSetMarkSilent(void) {
	markRingPush(E.buf);
	E.buf->markx = E.buf->cx;
	E.buf->marky = E.buf->cy;
	/* mark_active intentionally left unchanged (typically 0) */
	if (E.buf->marky >= E.buf->numrows) {
		E.buf->marky = E.buf->numrows - 1;
		E.buf->markx = E.buf->row[E.buf->marky].size;
	}
}

static void editorClearMarkQuiet(void) {
	E.buf->mark_active = 0;
}

void editorDeactivateMark(void) {
	E.buf->mark_active = 0;
}

void editorClearMark(void) {
	editorClearMarkQuiet();
	editorSetStatusMessage(msg_mark_cleared);
}

void editorPopMark(void) {
	struct editorBuffer *buf = E.buf;

	/*
	 * Emacs set-mark-command with arg does two things:
	 *   1. (goto-char (mark))  — move point to the current mark
	 *   2. (pop-mark)          — rotate the ring into the mark
	 *
	 * pop-mark: append current mark to end of ring, then set mark
	 * to the first (oldest) ring entry and remove it from the ring.
	 */

	/* Step 1: goto mark */
	if (buf->markx < 0 || buf->marky < 0) {
		editorSetStatusMessage(msg_no_mark_set);
		return;
	}

	int old_cx = buf->cx;
	int old_cy = buf->cy;

	buf->cx = buf->markx;
	buf->cy = buf->marky;

	/* Clamp */
	if (buf->cy >= buf->numrows)
		buf->cy = buf->numrows > 0 ? buf->numrows - 1 : 0;
	if (buf->cy < buf->numrows && buf->cx > buf->row[buf->cy].size)
		buf->cx = buf->row[buf->cy].size;

	/* Step 2: pop-mark — rotate ring into the mark.  */
	if (buf->mark_ring_len > 0) {
		/* Newest entry index */
		int n = (buf->mark_ring_idx - 1 + MARK_RING_SIZE) %
			MARK_RING_SIZE;

		/* This becomes the new mark */
		int new_cx = buf->mark_ring[n].cx;
		int new_cy = buf->mark_ring[n].cy;

		/* Shift all entries one position toward newest,
		 * opening a slot at the oldest position for
		 * the current mark. */
		int oldest = (buf->mark_ring_idx - buf->mark_ring_len +
			      MARK_RING_SIZE) %
			     MARK_RING_SIZE;
		for (int i = n; i != oldest;) {
			int prev = (i - 1 + MARK_RING_SIZE) % MARK_RING_SIZE;
			buf->mark_ring[i] = buf->mark_ring[prev];
			i = prev;
		}

		/* Put current mark at the oldest (back) position */
		buf->mark_ring[oldest].cx = buf->markx;
		buf->mark_ring[oldest].cy = buf->marky;

		/* mark_ring_idx and mark_ring_len unchanged */

		buf->markx = new_cx;
		buf->marky = new_cy;
	}

	buf->mark_active = 0;

	if (buf->cx == old_cx && buf->cy == old_cy)
		editorSetStatusMessage(msg_mark_popped);
}

void editorToggleRectangleMode(void) {
	E.buf->rectangle_mode = !E.buf->rectangle_mode;
	if (E.buf->rectangle_mode) {
		editorSetStatusMessage(msg_rectangle_on);
	} else {
		editorSetStatusMessage(msg_rectangle_off);
	}
}

void editorMarkBuffer(void) {
	if (E.buf->numrows > 0) {
		E.buf->cy = E.buf->numrows;
		E.buf->cx = E.buf->row[--E.buf->cy].size;
		editorSetMark();
		E.buf->cy = 0;
		E.buf->cx = 0;
	}
}

int markInvalidSilent(void) {
	return (E.buf->markx < 0 || E.buf->marky < 0 || E.buf->numrows == 0 ||
		E.buf->marky >= E.buf->numrows ||
		E.buf->markx > (E.buf->row[E.buf->marky].size) ||
		(E.buf->markx == E.buf->cx && E.buf->cy == E.buf->marky));
}

int markInvalid(void) {
	int ret = markInvalidSilent();

	if (ret) {
		editorSetStatusMessage(msg_mark_invalid);
	}

	return ret;
}

static void normalizeRegion(struct editorBuffer *buf) {
	/* Put cx,cy first */
	if (buf->cy > buf->marky ||
	    (buf->cy == buf->marky && buf->cx > buf->markx)) {
		int swapx, swapy;
		swapx = buf->cx;
		swapy = buf->cy;
		buf->cy = buf->marky;
		buf->cx = buf->markx;
		buf->markx = swapx;
		buf->marky = swapy;
	}
	/* Make sure mark is not outside buffer */
	if (buf->marky >= buf->numrows) {
		buf->marky = buf->numrows - 1;
		buf->markx = buf->row[buf->marky].size;
	}
}

/* Bounded append to undo data: append at most 'ncopy' bytes from 'src'
 * to the undo record, respecting both the source length and the
 * destination capacity.  Always NUL-terminates. */
static void undoAppendBounded(struct editorUndo *u, const uint8_t *src,
			      int src_len, int ncopy) {
	if (ncopy > src_len)
		ncopy = src_len;
	int dlen = strlen((char *)u->data);
	int avail = u->datasize - dlen - 1;
	if (ncopy > avail)
		ncopy = avail;
	if (ncopy > 0) {
		memcpy(&u->data[dlen], src, ncopy);
		u->data[dlen + ncopy] = 0;
	}
}

/* Normalise rectangle columns so topx <= botx.  Also sets up
 * buf->cx, buf->cy, buf->markx, buf->marky for the rectangle. */
static void normalizeRectCols(struct editorBuffer *buf, int *topx, int *topy,
			      int *botx, int *boty) {
	*boty = buf->marky;
	*topy = buf->cy;
	if (buf->cx > buf->markx) {
		*topx = buf->markx;
		*botx = buf->cx;
	} else {
		*botx = buf->markx;
		*topx = buf->cx;
	}
	buf->cx = *topx;
	buf->cy = *topy;
	buf->marky = *boty;
	if (*botx > buf->row[*boty].size)
		buf->markx = buf->row[*boty].size;
	else
		buf->markx = *botx;
}

void editorDeleteRange(struct editorBuffer *buf, int startx, int starty,
		       int endx, int endy, int add_to_kill_ring) {
	/* Normalise: ensure start comes before end */
	if (starty > endy || (starty == endy && startx > endx)) {
		int tx = startx, ty = starty;
		startx = endx;
		starty = endy;
		endx = tx;
		endy = ty;
	}

	/* Clamp end position within buffer */
	if (endy >= buf->numrows) {
		endy = buf->numrows - 1;
		endx = buf->row[endy].size;
	}

	/* Nothing to delete if start == end */
	if (startx == endx && starty == endy)
		return;

	/* Collect text between (startx, starty) and (endx, endy) using
	 * local variables — do NOT move buf->cx or buf->cy during
	 * collection. */
	int regionSize = 32;
	uint8_t *collected = xmalloc(regionSize);
	int cpos = 0;
	int lx = startx;
	int ly = starty;
	while (!(ly == endy && lx == endx)) {
		if (lx >= buf->row[ly].size) {
			collected[cpos++] = '\n';
			ly++;
			lx = 0;
		} else {
			collected[cpos++] = buf->row[ly].chars[lx];
			lx++;
		}
		if (cpos >= regionSize - 2) {
			regionSize *= 2;
			collected = xrealloc(collected, regionSize);
		}
	}
	collected[cpos] = 0;

	/* Kill ring */
	if (add_to_kill_ring) {
		clearEditorText(&E.kill);
		E.kill.str = (uint8_t *)xstrdup((char *)collected);
		addToKillRing((char *)collected, 0, 0, 0);
	}

	/* Undo record */
	clearRedos(buf);

	struct editorUndo *new = newUndo();
	new->startx = startx;
	new->starty = starty;
	new->endx = endx;
	new->endy = endy;
	free(new->data);
	new->datalen = cpos;
	new->datasize = cpos + 1;
	new->data = xmalloc(new->datasize);
	memcpy(new->data, collected, cpos);
	new->data[cpos] = 0;
	new->append = 0;
	new->delete = 1;
	pushUndo(buf, new);

	free(collected);

	/* Adjust tracked points BEFORE the mutation while row structure
	 * is still intact — matches the contract used by bulkDelete. */
	adjustAllPoints(buf, startx, starty, endx, endy, 1);

	/* Splice rows — same logic as the old editorKillRegion */
	struct erow *row = &buf->row[starty];
	if (starty == endy) {
		memmove(&row->chars[startx], &row->chars[endx],
			row->size - endx);
		row->size -= endx - startx;
		row->chars[row->size] = 0;
	} else {
		for (int i = starty + 1; i < endy; i++) {
			editorDelRow(buf, starty + 1);
		}
		struct erow *last = &buf->row[starty + 1];
		row->size = startx;
		row->size += last->size - endx;
		row->chars = xrealloc(row->chars, row->size + 1);
		memcpy(&row->chars[startx], &last->chars[endx],
		       last->size - endx);
		row->chars[row->size] = '\0';
		editorDelRow(buf, starty + 1);
	}

	buf->dirty = 1;
	editorUpdateBuffer(buf);

	/* Set cursor to start of deleted range */
	buf->cx = startx;
	buf->cy = starty;
}

void editorKillRegion(struct editorConfig *ed, struct editorBuffer *buf) {
	(void)ed;
	if (buf->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	if (markInvalid())
		return;
	editorDeleteRange(buf, buf->cx, buf->cy, buf->markx, buf->marky, 1);
}

void editorCopyRegion(struct editorConfig *ed, struct editorBuffer *buf) {
	if (markInvalid())
		return;
	int origCx = buf->cx;
	int origCy = buf->cy;
	int origMarkx = buf->markx;
	int origMarky = buf->marky;
	normalizeRegion(buf);
	clearEditorText(&ed->kill);
	int regionSize = 32;
	ed->kill.str = xmalloc(regionSize);

	int killpos = 0;
	/* Use local variables for iteration — never touch buf->cx/cy */
	int lx = buf->cx;
	int ly = buf->cy;
	while (!(ly == buf->marky && lx == buf->markx)) {
		if (lx >= buf->row[ly].size) {
			ly++;
			lx = 0;
			ed->kill.str[killpos++] = '\n';
		} else {
			ed->kill.str[killpos++] = buf->row[ly].chars[lx];
			lx++;
		}

		if (killpos >= regionSize - 2) {
			regionSize *= 2;
			ed->kill.str = xrealloc(ed->kill.str, regionSize);
		}
	}
	ed->kill.str[killpos] = 0;

	addToKillRing((char *)ed->kill.str, 0, 0, 0);

	buf->cx = origCx;
	buf->cy = origCy;
	buf->markx = origMarkx;
	buf->marky = origMarky;
}

void editorYank(struct editorConfig *ed, struct editorBuffer *buf, int count) {
	if (buf->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	if (ed->kill.str == NULL) {
		editorSetStatusMessage(msg_kill_ring_empty);
		return;
	}

	/* Numeric argument selects which kill ring entry to yank.
	 * 0 or 1 = most recent, 2 = second most recent, etc.
	 * Matches GNU Emacs C-y behavior. */
	if (count > 1) {
		int idx = ed->kill_history.count - count;
		if (idx < 0)
			idx = 0;
		struct historyEntry *entry =
			getHistoryAt(&ed->kill_history, idx);
		if (!entry) {
			editorSetStatusMessage(msg_kill_ring_empty);
			return;
		}
		clearEditorText(&ed->kill);
		ed->kill.str = (uint8_t *)xstrdup(entry->str);
		ed->kill.is_rectangle = entry->is_rectangle;
		ed->kill.rect_width = entry->rect_width;
		ed->kill.rect_height = entry->rect_height;

		/* If the selected entry is a rectangle, delegate */
		if (entry->is_rectangle) {
			ed->kill_ring_pos = idx;
			editorYankRectangle(ed, buf);
			return;
		}
	}

	int killLen = strlen((char *)ed->kill.str);

	clearRedos(buf);

	struct editorUndo *new = newUndo();
	new->startx = buf->cx;
	new->starty = buf->cy;
	free(new->data);
	new->datalen = killLen;
	new->datasize = new->datalen + 1;
	new->data = xmalloc(new->datasize);
	emil_strlcpy(new->data, ed->kill.str, new->datasize);
	new->append = 0;

	/* Compute end position from the kill text */
	int ex = buf->cx;
	int ey = buf->cy;
	for (int i = 0; i < killLen; i++) {
		if (ed->kill.str[i] == '\n') {
			ey++;
			ex = 0;
		} else {
			ex++;
		}
	}
	new->endx = ex;
	new->endy = ey;
	pushUndo(buf, new);

	/* bulkInsert handles the row manipulation and calls
	 * adjustAllPoints internally. */
	bulkInsert(buf, buf->cx, buf->cy, ed->kill.str, killLen);

	buf->cx = ex;
	buf->cy = ey;

	buf->dirty = 1;
	editorUpdateBuffer(buf);

	/* Set kill ring position so M-y continues from here */
	if (count > 1) {
		ed->kill_ring_pos = ed->kill_history.count - count;
		if (ed->kill_ring_pos < 0)
			ed->kill_ring_pos = 0;
	} else {
		ed->kill_ring_pos = ed->kill_history.count > 0 ?
					    ed->kill_history.count - 1 :
					    0;
	}
}

void editorYankPop(struct editorConfig *ed, struct editorBuffer *buf) {
	if (ed->kill_history.count == 0) {
		editorSetStatusMessage(msg_kill_ring_empty);
		return;
	}

	if (ed->kill_ring_pos < 0) {
		editorSetStatusMessage(msg_not_after_yank);
		return;
	}

	if (buf->undo == NULL || buf->undo->delete != 0) {
		editorSetStatusMessage(msg_not_after_yank);
		return;
	}

	editorDoUndo(buf, 1);

	ed->kill_ring_pos--;
	if (ed->kill_ring_pos < 0) {
		ed->kill_ring_pos = ed->kill_history.count - 1;
	}

	struct historyEntry *entry =
		getHistoryAt(&ed->kill_history, ed->kill_ring_pos);
	if (entry) {
		clearEditorText(&ed->kill);
		ed->kill.str = (uint8_t *)xstrdup(entry->str);
		ed->kill.is_rectangle = entry->is_rectangle;
		ed->kill.rect_width = entry->rect_width;
		ed->kill.rect_height = entry->rect_height;
		int saved_pos = ed->kill_ring_pos;
		if (entry->is_rectangle)
			editorYankRectangle(ed, buf);
		else
			editorYank(ed, buf, 1);
		ed->kill_ring_pos = saved_pos;
	} else {
		editorSetStatusMessage(msg_no_more_kill_entries);
	}
}

void editorTransformRange(struct editorConfig *ed, struct editorBuffer *buf,
			  int startx, int starty, int endx, int endy,
			  uint8_t *(*transformer)(uint8_t *)) {
	if (buf->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	/* Normalize: put start before end */
	if (starty > endy || (starty == endy && startx > endx)) {
		int tx = startx, ty = starty;
		startx = endx;
		starty = endy;
		endx = tx;
		endy = ty;
	}

	/* Set up mark/cursor for the kill/yank machinery */
	buf->cx = startx;
	buf->cy = starty;
	buf->markx = endx;
	buf->marky = endy;

	uint8_t *okill = saveKill(ed);
	editorKillRegion(ed, buf);

	uint8_t *input = ed->kill.str;
	uint8_t *transformed = transformer(input);
	free(ed->kill.str);
	ed->kill.str = transformed;
	editorYank(ed, buf, 1);
	buf->undo->paired = 1;

	restoreKill(ed, okill);
}

void editorTransformRegion(struct editorConfig *ed, struct editorBuffer *buf,
			   uint8_t *(*transformer)(uint8_t *)) {
	if (buf->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	if (markInvalid())
		return;
	normalizeRegion(buf);

	editorTransformRange(ed, buf, buf->cx, buf->cy, buf->markx, buf->marky,
			     transformer);
}

void editorReplaceRegex(struct editorConfig *ed, struct editorBuffer *buf) {
	if (markInvalid())
		return;
	normalizeRegion(buf);

	const char *cancel = "Canceled regex-replace.";
	int madeReplacements = 0;

	uint8_t *regex =
		editorPrompt(buf, "Regex replace: %s", PROMPT_BASIC, NULL);
	if (regex == NULL) {
		editorSetStatusMessage(cancel);
		return;
	}

	char prompt[64];
	snprintf(prompt, sizeof(prompt), "Regex replace %.35s with: %%s",
		 regex);
	uint8_t *repl =
		editorPrompt(buf, (uint8_t *)prompt, PROMPT_BASIC, NULL);
	if (repl == NULL) {
		free(regex);
		editorSetStatusMessage(cancel);
		return;
	}
	int replen = strlen((char *)repl);

	/* Compile the regex */
	regex_t pattern;
	regmatch_t matches[1];
	int regcomp_result = regcomp(&pattern, (char *)regex, REG_EXTENDED);
	if (regcomp_result != 0) {
		char error_msg[256];
		regerror(regcomp_result, &pattern, error_msg,
			 sizeof(error_msg));
		editorSetStatusMessage(msg_regex_error, error_msg);
		free(regex);
		free(repl);
		return;
	}

	uint8_t *okill = saveKill(ed);
	editorCopyRegion(ed, buf);

	/* This is a transformation, so create a delete undo. However, we're not
	 * actually doing any deletion yet in this case. */
	struct editorUndo *new = newUndo();
	new->startx = buf->cx;
	new->starty = buf->cy;
	new->endx = buf->markx;
	new->endy = buf->marky;
	new->datalen = strlen((char *)ed->kill.str);
	if (new->datasize < new->datalen + 1) {
		new->datasize = new->datalen + 1;
		new->data = xrealloc(new->data, new->datasize);
	}
	for (int i = 0; i < new->datalen; i++) {
		new->data[i] = ed->kill.str[i];
	}
	new->data[new->datalen] = 0;
	new->append = 0;
	new->delete = 1;
	pushUndo(buf, new);

	/* Create insert undo */
	new = newUndo();
	new->startx = buf->cx;
	new->starty = buf->cy;
	new->endy = buf->marky;
	new->datalen = buf->undo->datalen;
	if (new->datasize < new->datalen + 1) {
		new->datasize = new->datalen + 1;
		new->data = xrealloc(new->data, new->datasize);
	}
	new->append = 0;
	new->delete = 0;
	new->paired = 1;
	pushUndo(buf, new);

	for (int i = buf->cy; i <= buf->marky; i++) {
		struct erow *row = &buf->row[i];
		int regexec_result =
			regexec(&pattern, (char *)row->chars, 1, matches, 0);
		int match_idx = (regexec_result == 0) ? matches[0].rm_so : -1;
		int match_length = (regexec_result == 0) ? (matches[0].rm_eo -
							    matches[0].rm_so) :
							   0;
		if (i != 0)
			emil_strlcat((char *)new->data, "\n", new->datasize);
		if (match_idx < 0) {
			if (buf->cy == buf->marky) {
				emil_strlcat((char *)new->data,
					     (char *)&row->chars[buf->cx],
					     new->datasize);
			} else if (i == buf->cy) {
				emil_strlcat((char *)new->data,
					     (char *)&row->chars[buf->cx],
					     new->datasize);
			} else if (i == buf->marky) {
				emil_strlcat((char *)new->data,
					     (char *)row->chars, new->datasize);
			} else {
				emil_strlcat((char *)new->data,
					     (char *)row->chars, new->datasize);
			}
			continue;
		} else if (i == buf->cy && match_idx < buf->cx) {
			emil_strlcat((char *)new->data,
				     (char *)&row->chars[buf->cx],
				     new->datasize);
			continue;
		} else if (i == buf->marky &&
			   match_idx + match_length > buf->markx) {
			emil_strlcat((char *)new->data, (char *)row->chars,
				     new->datasize);
			continue;
		}
		madeReplacements++;
		/* Replace row data */
		row = &buf->row[i];
		int extra = replen - match_length;
		if (extra > 0) {
			row->chars =
				xrealloc(row->chars, row->size + 1 + extra);
			new->datasize += extra;
			new->data = xrealloc(new->data, new->datasize);
		}
		memmove(&row->chars[match_idx + replen],
			&row->chars[match_idx + match_length],
			row->size - (match_idx + match_length));
		memcpy(&row->chars[match_idx], repl, replen);
		row->size += extra;
		row->chars[row->size] = 0;
		if (buf->cy == buf->marky) {
			buf->markx += extra;
			emil_strlcat((char *)new->data,
				     (char *)&row->chars[buf->cx],
				     new->datasize);
		} else if (i == buf->cy) {
			emil_strlcat((char *)new->data,
				     (char *)&row->chars[buf->cx],
				     new->datasize);
		} else if (i == buf->marky) {
			buf->markx += extra;
			emil_strlcat((char *)new->data, (char *)row->chars,
				     new->datasize);
		} else {
			emil_strlcat((char *)new->data, (char *)row->chars,
				     new->datasize);
		}
	}
	/* Now take care of insert undo */
	new->data[new->datasize - 1] = 0;
	new->datalen = strlen((char *)new->data);
	new->endx = buf->markx;

	buf->cx = new->endx;
	buf->cy = new->endy;

	editorUpdateBuffer(buf);
	regfree(&pattern);
	free(regex);
	free(repl);

	restoreKill(ed, okill);

	editorSetStatusMessage(msg_replaced_n, madeReplacements);
}

void editorStringRectangle(struct editorConfig *ed, struct editorBuffer *buf) {
	if (markInvalid())
		return;

	uint8_t *string = editorPrompt(buf, (uint8_t *)"String rectangle: %s",
				       PROMPT_BASIC, NULL);
	if (string == NULL) {
		editorSetStatusMessage(msg_canceled);
		return;
	}

	uint8_t *okill = saveKill(ed);

	normalizeRegion(buf);

	int slen = strlen((char *)string);
	int topx, topy, botx, boty;
	normalizeRectCols(buf, &topx, &topy, &botx, &boty);
	int rwidth = botx - topx;
	int extra = slen - rwidth; /* new bytes per line */

	editorCopyRegion(ed, buf);
	clearRedos(buf);

	/* This is mostly a normal kill-region type undo. */
	struct editorUndo *new = newUndo();
	new->startx = buf->cx;
	new->starty = buf->cy;
	new->endx = buf->markx;
	new->endy = buf->marky;
	free(new->data);
	new->datalen = strlen((char *)ed->kill.str);
	new->datasize = new->datalen + 1;
	new->data = xmalloc(new->datasize);
	for (int i = 0; i < new->datalen; i++) {
		new->data[i] = ed->kill.str[i];
	}
	new->data[new->datalen] = 0;
	new->append = 0;
	new->delete = 1;
	pushUndo(buf, new);

	/* Undo for a yank region */
	new = newUndo();
	new->startx = topx;
	new->starty = topy;
	new->endx = botx + extra;
	new->endy = boty;
	free(new->data);
	new->datalen = 0;
	if (extra > 0) {
		new->datasize = strlen((char *)ed->kill.str) +
				(extra * ((boty - topy) + 1)) + 1;
	} else {
		new->datasize = strlen((char *)ed->kill.str);
	}
	new->data = xmalloc(new->datasize);
	new->data[0] = 0;
	new->append = 0;
	new->paired = 1;
	pushUndo(buf, new);

	/*
	 * We need to do the row modifying operation in three stages
	 * because the undo data we need to copy is slightly different:
	 * --RRRRXXX // Where - is don't copy,
	 * XXRRRRXXX // R is the replacement string,
	 * XXRRRR--- // and X is extra data.
	 */
	/* First, topy */
	struct erow *row = &buf->row[topy];
	if (row->size < botx) {
		row->chars = xrealloc(row->chars, botx + 1);
		memset(&row->chars[row->size], ' ', botx - row->size);
		row->size = botx;
		/* Better safe than sorry */
		new->datasize += row->size + 1;
		new->data = xrealloc(new->data, new->datasize);
	}
	if (extra > 0) {
		row->chars = xrealloc(row->chars, row->size + 1 + extra);
	}
	memmove(&row->chars[topx + slen], &row->chars[botx], row->size - botx);
	memcpy(&row->chars[topx], string, slen);
	row->size += extra;
	row->chars[row->size] = 0;
	/* Adjust for the column replacement: delete old range, insert new */
	adjustAllPoints(buf, topx, topy, botx, topy, 1);
	if (slen > 0)
		adjustAllPoints(buf, topx, topy, topx + slen, topy, 0);
	if (boty == topy) {
		emil_strlcat((char *)new->data, (char *)string, new->datasize);
	} else {
		emil_strlcat((char *)new->data, (char *)&row->chars[topx],
			     new->datasize);
	}

	for (int i = topy + 1; i < boty; i++) {
		emil_strlcat((char *)new->data, "\n", new->datasize);
		/* Next, middle lines */
		row = &buf->row[i];
		if (row->size < botx) {
			row->chars = xrealloc(row->chars, botx + 1);
			memset(&row->chars[row->size], ' ', botx - row->size);
			row->size = botx;
			new->datasize += row->size + 1;
			new->data = xrealloc(new->data, new->datasize);
		}
		if (extra > 0) {
			row->chars =
				xrealloc(row->chars, row->size + 1 + extra);
		}
		memmove(&row->chars[topx + slen], &row->chars[botx],
			row->size - botx);
		memcpy(&row->chars[topx], string, slen);
		row->size += extra;
		row->chars[row->size] = 0;
		adjustAllPoints(buf, topx, i, botx, i, 1);
		if (slen > 0)
			adjustAllPoints(buf, topx, i, topx + slen, i, 0);
		emil_strlcat((char *)new->data, (char *)row->chars,
			     new->datasize);
	}

	/* Finally, end line */
	if (topy != boty) {
		emil_strlcat((char *)new->data, "\n", new->datasize);
		row = &buf->row[boty];
		if (row->size < botx) {
			row->chars = xrealloc(row->chars, botx + 1);
			memset(&row->chars[row->size], ' ', botx - row->size);
			row->size = botx;
			new->datasize += row->size + 1;
			new->data = xrealloc(new->data, new->datasize);
		}
		if (extra > 0) {
			row->chars =
				xrealloc(row->chars, row->size + 1 + extra);
		}
		memmove(&row->chars[topx + slen], &row->chars[botx],
			row->size - botx);
		memcpy(&row->chars[topx], string, slen);
		row->size += extra;
		row->chars[row->size] = 0;
		adjustAllPoints(buf, topx, boty, botx, boty, 1);
		if (slen > 0)
			adjustAllPoints(buf, topx, boty, topx + slen, boty, 0);
		undoAppendBounded(new, row->chars, row->size, botx + extra);
	}
	new->datalen = strlen((char *)new->data);

	buf->dirty = 1;
	editorUpdateBuffer(buf);
	editorClearMarkQuiet();
	restoreKill(ed, okill);
}

void editorCopyRectangle(struct editorConfig *ed, struct editorBuffer *buf) {
	if (markInvalid())
		return;
	normalizeRegion(buf);

	int topx, topy, botx, boty;
	normalizeRectCols(buf, &topx, &topy, &botx, &boty);
	int rw = botx - topx;
	int rh = (boty - topy) + 1;

	clearEditorText(&ed->kill);
	ed->kill.str = xcalloc((rw * rh) + 1, 1);
	ed->kill.is_rectangle = 1;
	ed->kill.rect_width = rw;
	ed->kill.rect_height = rh;

	/* First, topy */
	int idx = 0;
	struct erow *row = &buf->row[topy + idx];
	if (row->size < botx) {
		memset(&ed->kill.str[idx * rw], ' ', rw);
		if (row->size > botx - rw) {
			strncpy((char *)&ed->kill.str[idx * rw],
				(char *)&row->chars[botx - rw],
				row->size - (botx - rw));
		}
	} else {
		strncpy((char *)&ed->kill.str[idx * rw],
			(char *)&row->chars[botx - rw], rw);
	}
	idx++;

	while ((topy + idx) < boty) {
		/* Middle lines */
		row = &buf->row[topy + idx];

		if (row->size < botx) {
			memset(&ed->kill.str[idx * rw], ' ', rw);
			if (row->size > botx - rw) {
				strncpy((char *)&ed->kill.str[idx * rw],
					(char *)&row->chars[botx - rw],
					row->size - (botx - rw));
			}
		} else {
			strncpy((char *)&ed->kill.str[idx * rw],
				(char *)&row->chars[botx - rw], rw);
		}

		idx++;
	}

	/* finally, end line */
	if (topy != boty) {
		row = &buf->row[topy + idx];

		if (row->size < botx) {
			memset(&ed->kill.str[idx * rw], ' ', rw);
			if (row->size > botx - rw) {
				strncpy((char *)&ed->kill.str[idx * rw],
					(char *)&row->chars[botx - rw],
					row->size - (botx - rw));
			}
		} else {
			strncpy((char *)&ed->kill.str[idx * rw],
				(char *)&row->chars[botx - rw], rw);
		}
	}

	addToKillRing((char *)ed->kill.str, 1, rw, rh);
	editorClearMarkQuiet();
}

void editorKillRectangle(struct editorConfig *ed, struct editorBuffer *buf) {
	if (markInvalid())
		return;
	normalizeRegion(buf);

	/* Phase 1: editorCopyRegion writes linear undo text into ed->kill */
	struct editorText saved = ed->kill;
	ed->kill = (struct editorText){ 0 };

	int topx, topy, botx, boty;
	normalizeRectCols(buf, &topx, &topy, &botx, &boty);
	int rw = botx - topx;
	int rh = (boty - topy) + 1;

	editorCopyRegion(ed, buf);
	clearRedos(buf);

	/* Temporary flat buffer for extracted rectangle columns */
	uint8_t *rectBuf = xcalloc((rw * rh) + 1, 1);

	struct editorUndo *new = newUndo();
	new->startx = buf->cx;
	new->starty = buf->cy;
	new->endx = buf->markx;
	new->endy = buf->marky;
	free(new->data);
	new->datalen = strlen((char *)ed->kill.str);
	new->datasize = new->datalen + 1;
	new->data = xmalloc(new->datasize);
	for (int i = 0; i < new->datalen; i++) {
		new->data[i] = ed->kill.str[i];
	}
	new->data[new->datalen] = 0;
	new->append = 0;
	new->delete = 1;
	pushUndo(buf, new);

	/* This is technically a transformation, so we need paired undos.
	 * Use a safe upper bound for initial size — the linear region
	 * text length is always sufficient since the residual content
	 * after rectangle column removal is smaller. */
	new = newUndo();
	new->startx = topx;
	new->starty = topy;
	new->endx = botx - rw;
	new->endy = boty;
	free(new->data);
	int kill_len = strlen((char *)ed->kill.str);
	new->datalen = 0;
	new->datasize = kill_len + 1;
	new->data = xmalloc(new->datasize);
	new->data[0] = 0;
	new->append = 0;
	new->paired = 1;
	pushUndo(buf, new);

	/* First, topy */
	int idx = 0;
	struct erow *row = &buf->row[topy + idx];
	if (row->size < botx) {
		memset(&rectBuf[idx * rw], ' ', rw);
		if (row->size > botx - rw) {
			int old_size = row->size;
			strncpy((char *)&rectBuf[idx * rw],
				(char *)&row->chars[botx - rw],
				row->size - (botx - rw));
			row->size -= (row->size - (botx - rw));
			row->chars[row->size] = 0;
			adjustAllPoints(buf, topx, topy, old_size, topy, 1);
			if (boty != topy) {
				emil_strlcat((char *)new->data,
					     (char *)&row->chars[botx - rw],
					     new->datasize);
			}
		}
	} else {
		strncpy((char *)&rectBuf[idx * rw],
			(char *)&row->chars[botx - rw], rw);
		memcpy(&row->chars[topx], &row->chars[botx], row->size - botx);
		row->size -= rw;
		row->chars[row->size] = 0;
		adjustAllPoints(buf, topx, topy, botx, topy, 1);
		if (boty != topy) {
			emil_strlcat((char *)new->data,
				     (char *)&row->chars[topx], new->datasize);
		}
	}
	idx++;

	while ((topy + idx) < boty) {
		/* Middle lines */
		int cur_row = topy + idx;
		emil_strlcat((char *)new->data, "\n", new->datasize);
		row = &buf->row[cur_row];

		if (row->size < botx) {
			memset(&rectBuf[idx * rw], ' ', rw);
			if (row->size > botx - rw) {
				int old_size = row->size;
				strncpy((char *)&rectBuf[idx * rw],
					(char *)&row->chars[botx - rw],
					row->size - (botx - rw));
				row->size -= (row->size - (botx - rw));
				row->chars[row->size] = 0;
				adjustAllPoints(buf, topx, cur_row, old_size,
						cur_row, 1);
			}
		} else {
			strncpy((char *)&rectBuf[idx * rw],
				(char *)&row->chars[botx - rw], rw);
			memcpy(&row->chars[topx], &row->chars[botx],
			       row->size - botx);
			row->size -= rw;
			row->chars[row->size] = 0;
			adjustAllPoints(buf, topx, cur_row, botx, cur_row, 1);
		}

		emil_strlcat((char *)new->data, (char *)row->chars,
			     new->datasize);
		idx++;
	}

	/* Finally, end line */
	if (topy != boty) {
		int cur_row = topy + idx;
		emil_strlcat((char *)new->data, "\n", new->datasize);
		row = &buf->row[cur_row];

		if (row->size < botx) {
			memset(&rectBuf[idx * rw], ' ', rw);
			if (row->size > botx - rw) {
				int old_size = row->size;
				strncpy((char *)&rectBuf[idx * rw],
					(char *)&row->chars[botx - rw],
					row->size - (botx - rw));
				row->size -= (row->size - (botx - rw));
				row->chars[row->size] = 0;
				adjustAllPoints(buf, topx, cur_row, old_size,
						cur_row, 1);
			}
		} else {
			strncpy((char *)&rectBuf[idx * rw],
				(char *)&row->chars[botx - rw], rw);
			memcpy(&row->chars[topx], &row->chars[botx],
			       row->size - botx);
			row->size -= rw;
			row->chars[row->size] = 0;
			adjustAllPoints(buf, topx, cur_row, botx, cur_row, 1);
		}

		undoAppendBounded(new, row->chars, row->size, topx);
	}
	new->datalen = strlen((char *)new->data);

	buf->dirty = 1;
	editorUpdateBuffer(buf);
	editorClearMarkQuiet();

	/* Phase 2: overwrite ed->kill with rectangle data for the kill ring */
	/* TODO: editorCopyRegion could take an output parameter to eliminate
	 * this two-phase use of ed->kill */
	clearEditorText(&ed->kill);
	ed->kill.str = rectBuf;
	ed->kill.is_rectangle = 1;
	ed->kill.rect_width = rw;
	ed->kill.rect_height = rh;
	addToKillRing((char *)ed->kill.str, 1, rw, rh);

	/* Restore the saved kill for non-rectangle use */
	clearEditorText(&saved);
}

void editorYankRectangle(struct editorConfig *ed, struct editorBuffer *buf) {
	int rw = ed->kill.rect_width;
	int rh = ed->kill.rect_height;

	struct editorText saved = ed->kill;
	ed->kill = (struct editorText){ 0 };

	int topx, topy, botx, boty;
	topx = buf->cx;
	topy = buf->cy;
	botx = topx;
	boty = topy + rh - 1;
	char *string = xcalloc(rw + 1, 1);

	/* Snapshot original row content BEFORE any mutation so that
	 * the delete undo record captures the true pre-edit state.
	 * This fixes: #16 (extra-line undo) and #17 (space-padding
	 * undo) — both were caused by capturing undo data after the
	 * buffer had been partially modified. */
	int orig_numrows = buf->numrows;
	int snap_count = (boty < orig_numrows) ? rh : orig_numrows - topy;
	int *snap_sizes = xmalloc(snap_count * sizeof(int));
	uint8_t **snap_chars = xmalloc(snap_count * sizeof(uint8_t *));
	for (int i = 0; i < snap_count; i++) {
		struct erow *r = &buf->row[topy + i];
		snap_sizes[i] = r->size;
		snap_chars[i] = xmalloc(r->size + 1);
		memcpy(snap_chars[i], r->chars, r->size + 1);
	}

	/* Add extra rows if the rectangle extends past the buffer */
	int extralines = 0;
	while (boty >= buf->numrows) {
		editorInsertRow(buf, buf->numrows, "", 0);
		extralines++;
	}

	clearRedos(buf);

	/* Undo record 1 (bottom of stack): extra-lines insert.
	 * Records the newlines appended to extend the buffer. */
	if (extralines) {
		struct editorUndo *u = newUndo();
		u->starty = orig_numrows - 1;
		u->startx = buf->row[u->starty].size;
		u->endx = 0;
		u->endy = buf->numrows - 1;
		if (extralines >= u->datasize) {
			u->datasize = extralines + 1;
			u->data = xrealloc(u->data, u->datasize);
		}
		memset(u->data, '\n', extralines);
		u->data[extralines] = 0;
		u->datalen = extralines;
		u->append = 0;
		u->delete = 0;
		pushUndo(buf, u);

		adjustAllPoints(buf, u->startx, u->starty, u->endx, u->endy, 0);
	}

	/* Undo record 2: delete — stores the original row content so
	 * that undoing re-inserts the pre-edit text.  Built from the
	 * snapshot taken before any mutation. */
	int del_datasize = 32;
	for (int i = 0; i < snap_count; i++)
		del_datasize += snap_sizes[i] + 1;
	uint8_t *del_data = xmalloc(del_datasize);
	int del_pos = 0;
	for (int i = 0; i < snap_count; i++) {
		if (i > 0)
			del_data[del_pos++] = '\n';
		memcpy(&del_data[del_pos], snap_chars[i], snap_sizes[i]);
		del_pos += snap_sizes[i];
	}
	/* For extra lines that didn't exist, add empty lines */
	for (int i = snap_count; i < rh; i++)
		del_data[del_pos++] = '\n';
	del_data[del_pos] = 0;

	struct editorUndo *del_undo = newUndo();
	del_undo->startx = 0;
	del_undo->starty = topy;
	/* End position: end of the last original row, or end of last
	 * extra line */
	if (snap_count > 0) {
		del_undo->endx = snap_sizes[snap_count - 1];
		del_undo->endy = topy + snap_count - 1;
	} else {
		del_undo->endx = 0;
		del_undo->endy = topy;
	}
	if (extralines) {
		del_undo->endx = 0;
		del_undo->endy = boty;
	}
	free(del_undo->data);
	del_undo->data = del_data;
	del_undo->datalen = del_pos;
	del_undo->datasize = del_datasize;
	del_undo->append = 0;
	del_undo->delete = 1;
	del_undo->paired = extralines ? 1 : 0;
	pushUndo(buf, del_undo);

	/* Undo record 3 (top of stack): insert — will be filled with
	 * the post-mutation row content during the per-row loop. */
	struct editorUndo *ins_undo = newUndo();
	ins_undo->startx = 0;
	ins_undo->starty = topy;
	ins_undo->endx = 0; /* updated after loop */
	ins_undo->endy = boty;
	free(ins_undo->data);
	ins_undo->datalen = 0;
	ins_undo->datasize = del_datasize + (rw * rh) + 1;
	ins_undo->data = xmalloc(ins_undo->datasize);
	ins_undo->data[0] = 0;
	ins_undo->append = 0;
	ins_undo->paired = 1;
	pushUndo(buf, ins_undo);

	/* Free snapshot */
	for (int i = 0; i < snap_count; i++)
		free(snap_chars[i]);
	free(snap_chars);
	free(snap_sizes);

	/* Per-row rectangle insertion */
	for (int idx = 0; idx < rh; idx++) {
		int cur_row = topy + idx;
		struct erow *row = &buf->row[cur_row];

		if (idx > 0)
			emil_strlcat((char *)ins_undo->data, "\n",
				     ins_undo->datasize);

		strncpy(string, (char *)&saved.str[idx * rw], rw);

		/* Pad row with spaces if shorter than insertion column */
		if (row->size < botx) {
			row->chars = xrealloc(row->chars, botx + 1);
			memset(&row->chars[row->size], ' ', botx - row->size);
			row->size = botx;
			ins_undo->datasize += row->size + 1;
			ins_undo->data =
				xrealloc(ins_undo->data, ins_undo->datasize);
		}

		/* Make room and insert rectangle slice */
		if (rw > 0)
			row->chars = xrealloc(row->chars, row->size + 1 + rw);
		memmove(&row->chars[topx + rw], &row->chars[botx],
			row->size - botx);
		memcpy(&row->chars[topx], string, rw);
		row->size += rw;
		row->chars[row->size] = 0;
		row->cached_width = -1;

		if (rw > 0)
			adjustAllPoints(buf, topx, cur_row, topx + rw, cur_row,
					0);

		/* Append post-mutation row content to insert undo.
		 * For the last row, use bounded append for the
		 * partial-row undo convention. */
		if (idx == rh - 1 && topy != boty) {
			undoAppendBounded(ins_undo, row->chars, row->size,
					  row->size);
		} else {
			emil_strlcat((char *)ins_undo->data, (char *)row->chars,
				     ins_undo->datasize);
		}
	}
	ins_undo->datalen = strlen((char *)ins_undo->data);
	ins_undo->endx = buf->row[boty].size;

	free(string);

	buf->dirty = 1;
	editorUpdateBuffer(buf);
	editorClearMarkQuiet();
	clearEditorText(&ed->kill);
	ed->kill = saved;
}
