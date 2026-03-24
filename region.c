#include "region.h"
#include "adjust.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "history.h"
#include "message.h"
#include "prompt.h"
#include "undo.h"
#include "util.h"
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct config E;

void addToKillRing(const char *text, int is_rect, int rect_width,
		   int rect_height) {
	if (!text || strlen(text) == 0)
		return;

	addHistoryWithRect(&E.kill_history, text, is_rect, rect_width,
			   rect_height);
	E.kill_ring_pos = -1;

	/* text may point to E.kill.str, so copy before freeing. */
	uint8_t *copy = (uint8_t *)xstrdup(text);
	clearText(&E.kill);
	E.kill.str = copy;
	E.kill.is_rectangle = is_rect;
	E.kill.rect_width = rect_width;
	E.kill.rect_height = rect_height;
}

/* Save and restore the kill text around operations that temporarily
 * overwrite E.kill (transforms, rectangle ops). */
static uint8_t *saveKill(void) {
	if (E.kill.str == NULL)
		return NULL;
	return (uint8_t *)xstrdup((char *)E.kill.str);
}

static void restoreKill(uint8_t *saved) {
	free(E.kill.str);
	E.kill.str = saved;
}

/* Push the current mark position onto the mark ring (if mark is valid). */
static void markRingPush(void) {
	if (E.buf->markx < 0 || E.buf->marky < 0)
		return;
	E.buf->mark_ring[E.buf->mark_ring_idx].cx = E.buf->markx;
	E.buf->mark_ring[E.buf->mark_ring_idx].cy = E.buf->marky;
	E.buf->mark_ring_idx = (E.buf->mark_ring_idx + 1) % MARK_RING_SIZE;
	if (E.buf->mark_ring_len < MARK_RING_SIZE)
		E.buf->mark_ring_len++;
}

void setMark(void) {
	E.buf->rectangle_mode = 0;
	/* C-SPC C-SPC: if mark is already active at point, deactivate it.
	 * This lets you drop a mark for later pop-back without starting
	 * a visible selection. */
	if (E.buf->mark_active && E.buf->markx == E.buf->cx &&
	    E.buf->marky == E.buf->cy) {
		E.buf->mark_active = 0;
		setStatusMessage(msg_mark_cleared);
		return;
	}
	markRingPush();
	E.buf->markx = E.buf->cx;
	E.buf->marky = E.buf->cy;
	E.buf->mark_active = 1;
	setStatusMessage(msg_mark_set);
	if (E.buf->marky >= E.buf->numrows) {
		E.buf->marky = E.buf->numrows - 1;
		E.buf->markx = E.buf->row[E.buf->marky].size;
	}
}

/* Set mark at point, push old mark onto ring, but do NOT activate
 * (no highlighting) and do NOT print a message.  Used before jumps
 * like isearch, M-<, M->, goto-line, register-jump so the user
 * can pop back with C-u C-SPC. */
void setMarkSilent(void) {
	markRingPush();
	E.buf->markx = E.buf->cx;
	E.buf->marky = E.buf->cy;
	/* mark_active intentionally left unchanged (typically 0) */
	if (E.buf->marky >= E.buf->numrows) {
		E.buf->marky = E.buf->numrows - 1;
		E.buf->markx = E.buf->row[E.buf->marky].size;
	}
}

static void clearMarkQuiet(void) {
	E.buf->mark_active = 0;
}

void deactivateMark(void) {
	E.buf->mark_active = 0;
}

void clearMark(void) {
	clearMarkQuiet();
	setStatusMessage(msg_mark_cleared);
}

void popMark(void) {
	struct buffer *buf = E.buf;

	/*
	 * Emacs set-mark-command with arg does two things:
	 *   1. (goto-char (mark))  — move point to the current mark
	 *   2. (pop-mark)          — rotate the ring into the mark
	 *
	 * pop-mark: append current mark to end of ring, then set mark
	 * to the first (oldest) ring entry and remove it from the ring.
	 */

	/* Step 1: goto mark */
	if (E.buf->markx < 0 || E.buf->marky < 0) {
		setStatusMessage(msg_no_mark_set);
		return;
	}

	int old_cx = E.buf->cx;
	int old_cy = E.buf->cy;

	E.buf->cx = buf->markx;
	E.buf->cy = buf->marky;

	/* Clamp */
	if (E.buf->cy >= E.buf->numrows)
		buf->cy = E.buf->numrows > 0 ? E.buf->numrows - 1 : 0;
	if (E.buf->cy < E.buf->numrows &&
	    E.buf->cx > E.buf->row[E.buf->cy].size)
		E.buf->cx = E.buf->row[E.buf->cy].size;

	/* Step 2: pop-mark — rotate ring into the mark.  */
	if (E.buf->mark_ring_len > 0) {
		/* Newest entry index */
		int n = (E.buf->mark_ring_idx - 1 + MARK_RING_SIZE) %
			MARK_RING_SIZE;

		/* This becomes the new mark */
		int new_cx = E.buf->mark_ring[n].cx;
		int new_cy = E.buf->mark_ring[n].cy;

		/* Shift all entries one position toward newest,
		 * opening a slot at the oldest position for
		 * the current mark. */
		int oldest = (E.buf->mark_ring_idx - E.buf->mark_ring_len +
			      MARK_RING_SIZE) %
			     MARK_RING_SIZE;
		for (int i = n; i != oldest;) {
			int prev = (i - 1 + MARK_RING_SIZE) % MARK_RING_SIZE;
			E.buf->mark_ring[i] = E.buf->mark_ring[prev];
			i = prev;
		}

		/* Put current mark at the oldest (back) position */
		E.buf->mark_ring[oldest].cx = E.buf->markx;
		E.buf->mark_ring[oldest].cy = E.buf->marky;

		/* mark_ring_idx and mark_ring_len unchanged */

		E.buf->markx = new_cx;
		E.buf->marky = new_cy;
	}

	E.buf->mark_active = 0;

	if (E.buf->cx == old_cx && E.buf->cy == old_cy)
		setStatusMessage(msg_mark_popped);
}

void toggleRectangleMode(void) {
	E.buf->rectangle_mode = !E.buf->rectangle_mode;
	if (E.buf->rectangle_mode) {
		setStatusMessage(msg_rectangle_on);
	} else {
		setStatusMessage(msg_rectangle_off);
	}
}

void markBuffer(void) {
	if (E.buf->numrows > 0) {
		E.buf->cy = E.buf->numrows;
		E.buf->cx = E.buf->row[--E.buf->cy].size;
		setMark();
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
		setStatusMessage(msg_mark_invalid);
	}

	return ret;
}

static void normalizeRegion(void) {
	/* Put cx,cy first */
	if (E.buf->cy > E.buf->marky ||
	    (E.buf->cy == E.buf->marky && E.buf->cx > E.buf->markx)) {
		int swapx, swapy;
		swapx = E.buf->cx;
		swapy = E.buf->cy;
		E.buf->cy = E.buf->marky;
		E.buf->cx = E.buf->markx;
		E.buf->markx = swapx;
		E.buf->marky = swapy;
	}
	/* Make sure mark is not outside buffer */
	if (E.buf->marky >= E.buf->numrows) {
		E.buf->marky = E.buf->numrows - 1;
		E.buf->markx = E.buf->row[E.buf->marky].size;
	}
}

/* Bounded append to undo data: append at most 'ncopy' bytes from 'src'
 * to the undo record, respecting both the source length and the
 * destination capacity.  Always NUL-terminates. */
static void undoAppendBounded(struct undo *u, const uint8_t *src, int src_len,
			      int ncopy) {
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
static void normalizeRectCols(int *topx, int *topy, int *botx, int *boty) {
	*boty = E.buf->marky;
	*topy = E.buf->cy;
	if (E.buf->cx > E.buf->markx) {
		*topx = E.buf->markx;
		*botx = E.buf->cx;
	} else {
		*botx = E.buf->markx;
		*topx = E.buf->cx;
	}
	E.buf->cx = *topx;
	E.buf->cy = *topy;
	E.buf->marky = *boty;
	if (*botx > E.buf->row[*boty].size)
		E.buf->markx = E.buf->row[*boty].size;
	else
		E.buf->markx = *botx;
}

void deleteRange(int startx, int starty, int endx, int endy,
		 int add_to_kill_ring) {
	/* Normalise: ensure start comes before end */
	if (starty > endy || (starty == endy && startx > endx)) {
		int tx = startx, ty = starty;
		startx = endx;
		starty = endy;
		endx = tx;
		endy = ty;
	}

	/* Clamp end position within buffer */
	if (endy >= E.buf->numrows) {
		endy = E.buf->numrows - 1;
		endx = E.buf->row[endy].size;
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
		if (lx >= E.buf->row[ly].size) {
			collected[cpos++] = '\n';
			ly++;
			lx = 0;
		} else {
			collected[cpos++] = E.buf->row[ly].chars[lx];
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
		clearText(&E.kill);
		E.kill.str = (uint8_t *)xstrdup((char *)collected);
		addToKillRing((char *)collected, 0, 0, 0);
	}

	/* Undo record */
	clearRedos(E.buf);

	struct undo *new = newUndo();
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
	pushUndo(E.buf, new);

	free(collected);

	/* Adjust tracked points BEFORE the mutation while row structure
	 * is still intact — matches the contract used by bulkDelete. */
	adjustAllPoints(E.buf, startx, starty, endx, endy, 1);

	/* Splice rows — same logic as the old killRegion */
	struct erow *row = &E.buf->row[starty];
	if (starty == endy) {
		memmove(&row->chars[startx], &row->chars[endx],
			row->size - endx);
		row->size -= endx - startx;
		row->chars[row->size] = 0;
	} else {
		for (int i = starty + 1; i < endy; i++) {
			delRow(E.buf, starty + 1);
		}
		struct erow *last = &E.buf->row[starty + 1];
		row->size = startx;
		row->size += last->size - endx;
		row->chars = xrealloc(row->chars, row->size + 1);
		memcpy(&row->chars[startx], &last->chars[endx],
		       last->size - endx);
		row->chars[row->size] = '\0';
		delRow(E.buf, starty + 1);
	}

	E.buf->dirty = 1;
	updateBuffer(E.buf);

	/* Set cursor to start of deleted range */
	E.buf->cx = startx;
	E.buf->cy = starty;
}

void killRegion(void) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	if (markInvalid())
		return;
	deleteRange(E.buf->cx, E.buf->cy, E.buf->markx, E.buf->marky, 1);
}

void copyRegion(void) {
	if (markInvalid())
		return;
	int origCx = E.buf->cx;
	int origCy = E.buf->cy;
	int origMarkx = E.buf->markx;
	int origMarky = E.buf->marky;
	normalizeRegion();
	clearText(&E.kill);
	int regionSize = 32;
	E.kill.str = xmalloc(regionSize);

	int killpos = 0;
	/* Use local variables for iteration — never touch buf->cx/cy */
	int lx = E.buf->cx;
	int ly = E.buf->cy;
	while (!(ly == E.buf->marky && lx == E.buf->markx)) {
		if (lx >= E.buf->row[ly].size) {
			ly++;
			lx = 0;
			E.kill.str[killpos++] = '\n';
		} else {
			E.kill.str[killpos++] = E.buf->row[ly].chars[lx];
			lx++;
		}

		if (killpos >= regionSize - 2) {
			regionSize *= 2;
			E.kill.str = xrealloc(E.kill.str, regionSize);
		}
	}
	E.kill.str[killpos] = 0;

	addToKillRing((char *)E.kill.str, 0, 0, 0);

	E.buf->cx = origCx;
	E.buf->cy = origCy;
	E.buf->markx = origMarkx;
	E.buf->marky = origMarky;
}

void yank(int count) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	if (E.kill.str == NULL) {
		setStatusMessage(msg_kill_ring_empty);
		return;
	}

	/* Numeric argument selects which kill ring entry to yank.
	 * 0 or 1 = most recent, 2 = second most recent, etc.
	 * Matches GNU Emacs C-y behavior. */
	if (count < 1)
		count = 1;
	if (count > 1) {
		int idx = E.kill_history.count - count;
		if (idx < 0)
			idx = 0;
		struct historyEntry *entry = getHistoryAt(&E.kill_history, idx);
		if (!entry) {
			setStatusMessage(msg_kill_ring_empty);
			return;
		}
		clearText(&E.kill);
		E.kill.str = (uint8_t *)xstrdup(entry->str);
		E.kill.is_rectangle = entry->is_rectangle;
		E.kill.rect_width = entry->rect_width;
		E.kill.rect_height = entry->rect_height;

		/* If the selected entry is a rectangle, delegate */
		if (entry->is_rectangle) {
			E.kill_ring_pos = idx;
			yankRectangle();
			return;
		}
	}

	int killLen = strlen((char *)E.kill.str);

	clearRedos(E.buf);

	struct undo *new = newUndo();
	new->startx = E.buf->cx;
	new->starty = E.buf->cy;
	free(new->data);
	new->datalen = killLen;
	new->datasize = new->datalen + 1;
	new->data = xmalloc(new->datasize);
	emil_strlcpy(new->data, E.kill.str, new->datasize);
	new->append = 0;

	/* Compute end position from the kill text */
	int ex = E.buf->cx;
	int ey = E.buf->cy;
	for (int i = 0; i < killLen; i++) {
		if (E.kill.str[i] == '\n') {
			ey++;
			ex = 0;
		} else {
			ex++;
		}
	}
	new->endx = ex;
	new->endy = ey;
	pushUndo(E.buf, new);

	/* bulkInsert handles the row manipulation and calls
	 * adjustAllPoints internally. */
	bulkInsert(E.buf, E.buf->cx, E.buf->cy, E.kill.str, killLen);

	E.buf->cx = ex;
	E.buf->cy = ey;

	E.buf->dirty = 1;
	updateBuffer(E.buf);

	/* Set kill ring position so M-y continues from here */
	if (count > 1) {
		E.kill_ring_pos = E.kill_history.count - count;
		if (E.kill_ring_pos < 0)
			E.kill_ring_pos = 0;
	} else {
		E.kill_ring_pos =
			E.kill_history.count > 0 ? E.kill_history.count - 1 : 0;
	}
}

void yankPop(void) {
	if (E.kill_history.count == 0) {
		setStatusMessage(msg_kill_ring_empty);
		return;
	}

	if (E.kill_ring_pos < 0) {
		setStatusMessage(msg_not_after_yank);
		return;
	}

	if (E.buf->undo == NULL || E.buf->undo->delete != 0) {
		setStatusMessage(msg_not_after_yank);
		return;
	}

	doUndo(E.buf, 1);

	E.kill_ring_pos--;
	if (E.kill_ring_pos < 0) {
		E.kill_ring_pos = E.kill_history.count - 1;
	}

	struct historyEntry *entry =
		getHistoryAt(&E.kill_history, E.kill_ring_pos);
	if (entry) {
		clearText(&E.kill);
		E.kill.str = (uint8_t *)xstrdup(entry->str);
		E.kill.is_rectangle = entry->is_rectangle;
		E.kill.rect_width = entry->rect_width;
		E.kill.rect_height = entry->rect_height;
		int saved_pos = E.kill_ring_pos;
		if (entry->is_rectangle)
			yankRectangle();
		else
			yank(1);
		E.kill_ring_pos = saved_pos;
	} else {
		setStatusMessage(msg_no_more_kill_entries);
	}
}

void transformRange(int startx, int starty, int endx, int endy,
		    uint8_t *(*transformer)(uint8_t *)) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
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
	E.buf->cx = startx;
	E.buf->cy = starty;
	E.buf->markx = endx;
	E.buf->marky = endy;

	uint8_t *okill = saveKill();
	killRegion();

	uint8_t *input = E.kill.str;
	uint8_t *transformed = transformer(input);
	free(E.kill.str);
	E.kill.str = transformed;
	yank(1);
	E.buf->undo->paired = 1;

	restoreKill(okill);
}

void transformRegion(uint8_t *(*transformer)(uint8_t *)) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	if (markInvalid())
		return;
	normalizeRegion();

	transformRange(E.buf->cx, E.buf->cy, E.buf->markx, E.buf->marky,
		       transformer);
}

void replaceRegex(void) {
	if (markInvalid())
		return;
	normalizeRegion();

	const char *cancel = "Canceled regex-replace.";
	int madeReplacements = 0;

	uint8_t *regex =
		editorPrompt(E.buf, "Regex replace: %s", PROMPT_BASIC, NULL);
	if (regex == NULL) {
		setStatusMessage(cancel);
		return;
	}

	char prompt[64];
	snprintf(prompt, sizeof(prompt), "Regex replace %.35s with: %%s",
		 regex);
	uint8_t *repl =
		editorPrompt(E.buf, (uint8_t *)prompt, PROMPT_BASIC, NULL);
	if (repl == NULL) {
		free(regex);
		setStatusMessage(cancel);
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
		setStatusMessage(msg_regex_error, error_msg);
		free(regex);
		free(repl);
		return;
	}

	uint8_t *okill = saveKill();
	copyRegion();

	/* This is a transformation, so create a delete undo. However, we're not
	 * actually doing any deletion yet in this case. */
	struct undo *new = newUndo();
	new->startx = E.buf->cx;
	new->starty = E.buf->cy;
	new->endx = E.buf->markx;
	new->endy = E.buf->marky;
	new->datalen = strlen((char *)E.kill.str);
	if (new->datasize < new->datalen + 1) {
		new->datasize = new->datalen + 1;
		new->data = xrealloc(new->data, new->datasize);
	}
	for (int i = 0; i < new->datalen; i++) {
		new->data[i] = E.kill.str[i];
	}
	new->data[new->datalen] = 0;
	new->append = 0;
	new->delete = 1;
	pushUndo(E.buf, new);

	/* Create insert undo */
	new = newUndo();
	new->startx = E.buf->cx;
	new->starty = E.buf->cy;
	new->endy = E.buf->marky;
	new->datalen = E.buf->undo->datalen;
	if (new->datasize < new->datalen + 1) {
		new->datasize = new->datalen + 1;
		new->data = xrealloc(new->data, new->datasize);
	}
	new->append = 0;
	new->delete = 0;
	new->paired = 1;
	pushUndo(E.buf, new);

	for (int i = E.buf->cy; i <= E.buf->marky; i++) {
		struct erow *row = &E.buf->row[i];
		int regexec_result =
			regexec(&pattern, (char *)row->chars, 1, matches, 0);
		int match_idx = (regexec_result == 0) ? matches[0].rm_so : -1;
		int match_length = (regexec_result == 0) ? (matches[0].rm_eo -
							    matches[0].rm_so) :
							   0;
		if (i != 0)
			emil_strlcat((char *)new->data, "\n", new->datasize);
		if (match_idx < 0) {
			if (E.buf->cy == E.buf->marky) {
				emil_strlcat((char *)new->data,
					     (char *)&row->chars[E.buf->cx],
					     new->datasize);
			} else if (i == E.buf->cy) {
				emil_strlcat((char *)new->data,
					     (char *)&row->chars[E.buf->cx],
					     new->datasize);
			} else if (i == E.buf->marky) {
				emil_strlcat((char *)new->data,
					     (char *)row->chars, new->datasize);
			} else {
				emil_strlcat((char *)new->data,
					     (char *)row->chars, new->datasize);
			}
			continue;
		} else if (i == E.buf->cy && match_idx < E.buf->cx) {
			emil_strlcat((char *)new->data,
				     (char *)&row->chars[E.buf->cx],
				     new->datasize);
			continue;
		} else if (i == E.buf->marky &&
			   match_idx + match_length > E.buf->markx) {
			emil_strlcat((char *)new->data, (char *)row->chars,
				     new->datasize);
			continue;
		}
		madeReplacements++;
		/* Replace row data */
		row = &E.buf->row[i];
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
		if (E.buf->cy == E.buf->marky) {
			E.buf->markx += extra;
			emil_strlcat((char *)new->data,
				     (char *)&row->chars[E.buf->cx],
				     new->datasize);
		} else if (i == E.buf->cy) {
			emil_strlcat((char *)new->data,
				     (char *)&row->chars[E.buf->cx],
				     new->datasize);
		} else if (i == E.buf->marky) {
			E.buf->markx += extra;
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
	new->endx = E.buf->markx;

	E.buf->cx = new->endx;
	E.buf->cy = new->endy;

	updateBuffer(E.buf);
	regfree(&pattern);
	free(regex);
	free(repl);

	restoreKill(okill);

	setStatusMessage(msg_replaced_n, madeReplacements);
}

void stringRectangle(void) {
	if (markInvalid())
		return;

	uint8_t *string = editorPrompt(E.buf, (uint8_t *)"String rectangle: %s",
				       PROMPT_BASIC, NULL);
	if (string == NULL) {
		setStatusMessage(msg_canceled);
		return;
	}

	uint8_t *okill = saveKill();

	normalizeRegion();

	int slen = strlen((char *)string);
	int topx, topy, botx, boty;
	normalizeRectCols(&topx, &topy, &botx, &boty);
	int rwidth = botx - topx;
	int extra = slen - rwidth; /* new bytes per line */

	copyRegion();
	clearRedos(E.buf);

	/* This is mostly a normal kill-region type undo. */
	struct undo *new = newUndo();
	new->startx = E.buf->cx;
	new->starty = E.buf->cy;
	new->endx = E.buf->markx;
	new->endy = E.buf->marky;
	free(new->data);
	new->datalen = strlen((char *)E.kill.str);
	new->datasize = new->datalen + 1;
	new->data = xmalloc(new->datasize);
	for (int i = 0; i < new->datalen; i++) {
		new->data[i] = E.kill.str[i];
	}
	new->data[new->datalen] = 0;
	new->append = 0;
	new->delete = 1;
	pushUndo(E.buf, new);

	/* Undo for a yank region */
	new = newUndo();
	new->startx = topx;
	new->starty = topy;
	new->endx = botx + extra;
	new->endy = boty;
	free(new->data);
	new->datalen = 0;
	if (extra > 0) {
		new->datasize = strlen((char *)E.kill.str) +
				(extra * ((boty - topy) + 1)) + 1;
	} else {
		new->datasize = strlen((char *)E.kill.str);
	}
	new->data = xmalloc(new->datasize);
	new->data[0] = 0;
	new->append = 0;
	new->paired = 1;
	pushUndo(E.buf, new);

	/*
	 * We need to do the row modifying operation in three stages
	 * because the undo data we need to copy is slightly different:
	 * --RRRRXXX // Where - is don't copy,
	 * XXRRRRXXX // R is the replacement string,
	 * XXRRRR--- // and X is extra data.
	 */
	/* First, topy */
	struct erow *row = &E.buf->row[topy];
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
	adjustAllPoints(E.buf, topx, topy, botx, topy, 1);
	if (slen > 0)
		adjustAllPoints(E.buf, topx, topy, topx + slen, topy, 0);
	if (boty == topy) {
		emil_strlcat((char *)new->data, (char *)string, new->datasize);
	} else {
		emil_strlcat((char *)new->data, (char *)&row->chars[topx],
			     new->datasize);
	}

	for (int i = topy + 1; i < boty; i++) {
		emil_strlcat((char *)new->data, "\n", new->datasize);
		/* Next, middle lines */
		row = &E.buf->row[i];
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
		adjustAllPoints(E.buf, topx, i, botx, i, 1);
		if (slen > 0)
			adjustAllPoints(E.buf, topx, i, topx + slen, i, 0);
		emil_strlcat((char *)new->data, (char *)row->chars,
			     new->datasize);
	}

	/* Finally, end line */
	if (topy != boty) {
		emil_strlcat((char *)new->data, "\n", new->datasize);
		row = &E.buf->row[boty];
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
		adjustAllPoints(E.buf, topx, boty, botx, boty, 1);
		if (slen > 0)
			adjustAllPoints(E.buf, topx, boty, topx + slen, boty,
					0);
		undoAppendBounded(new, row->chars, row->size, botx + extra);
	}
	new->datalen = strlen((char *)new->data);

	E.buf->dirty = 1;
	updateBuffer(E.buf);
	clearMarkQuiet();
	restoreKill(okill);
}

void copyRectangle(void) {
	if (markInvalid())
		return;
	normalizeRegion();

	int topx, topy, botx, boty;
	normalizeRectCols(&topx, &topy, &botx, &boty);
	int rw = botx - topx;
	int rh = (boty - topy) + 1;

	clearText(&E.kill);
	E.kill.str = xcalloc((rw * rh) + 1, 1);
	E.kill.is_rectangle = 1;
	E.kill.rect_width = rw;
	E.kill.rect_height = rh;

	/* First, topy */
	int idx = 0;
	struct erow *row = &E.buf->row[topy + idx];
	if (row->size < botx) {
		memset(&E.kill.str[idx * rw], ' ', rw);
		if (row->size > botx - rw) {
			strncpy((char *)&E.kill.str[idx * rw],
				(char *)&row->chars[botx - rw],
				row->size - (botx - rw));
		}
	} else {
		strncpy((char *)&E.kill.str[idx * rw],
			(char *)&row->chars[botx - rw], rw);
	}
	idx++;

	while ((topy + idx) < boty) {
		/* Middle lines */
		row = &E.buf->row[topy + idx];

		if (row->size < botx) {
			memset(&E.kill.str[idx * rw], ' ', rw);
			if (row->size > botx - rw) {
				strncpy((char *)&E.kill.str[idx * rw],
					(char *)&row->chars[botx - rw],
					row->size - (botx - rw));
			}
		} else {
			strncpy((char *)&E.kill.str[idx * rw],
				(char *)&row->chars[botx - rw], rw);
		}

		idx++;
	}

	/* finally, end line */
	if (topy != boty) {
		row = &E.buf->row[topy + idx];

		if (row->size < botx) {
			memset(&E.kill.str[idx * rw], ' ', rw);
			if (row->size > botx - rw) {
				strncpy((char *)&E.kill.str[idx * rw],
					(char *)&row->chars[botx - rw],
					row->size - (botx - rw));
			}
		} else {
			strncpy((char *)&E.kill.str[idx * rw],
				(char *)&row->chars[botx - rw], rw);
		}
	}

	addToKillRing((char *)E.kill.str, 1, rw, rh);
	clearMarkQuiet();
}

void killRectangle(void) {
	if (markInvalid())
		return;
	normalizeRegion();

	/* Phase 1: copyRegion writes linear undo text into E.kill */
	struct text saved = E.kill;
	E.kill = (struct text){ 0 };

	int topx, topy, botx, boty;
	normalizeRectCols(&topx, &topy, &botx, &boty);
	int rw = botx - topx;
	int rh = (boty - topy) + 1;

	copyRegion();
	clearRedos(E.buf);

	/* Temporary flat buffer for extracted rectangle columns */
	uint8_t *rectBuf = xcalloc((rw * rh) + 1, 1);

	struct undo *new = newUndo();
	new->startx = E.buf->cx;
	new->starty = E.buf->cy;
	new->endx = E.buf->markx;
	new->endy = E.buf->marky;
	free(new->data);
	new->datalen = strlen((char *)E.kill.str);
	new->datasize = new->datalen + 1;
	new->data = xmalloc(new->datasize);
	for (int i = 0; i < new->datalen; i++) {
		new->data[i] = E.kill.str[i];
	}
	new->data[new->datalen] = 0;
	new->append = 0;
	new->delete = 1;
	pushUndo(E.buf, new);

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
	int kill_len = strlen((char *)E.kill.str);
	new->datalen = 0;
	new->datasize = kill_len + 1;
	new->data = xmalloc(new->datasize);
	new->data[0] = 0;
	new->append = 0;
	new->paired = 1;
	pushUndo(E.buf, new);

	/* First, topy */
	int idx = 0;
	struct erow *row = &E.buf->row[topy + idx];
	if (row->size < botx) {
		memset(&rectBuf[idx * rw], ' ', rw);
		if (row->size > botx - rw) {
			int old_size = row->size;
			strncpy((char *)&rectBuf[idx * rw],
				(char *)&row->chars[botx - rw],
				row->size - (botx - rw));
			row->size -= (row->size - (botx - rw));
			row->chars[row->size] = 0;
			adjustAllPoints(E.buf, topx, topy, old_size, topy, 1);
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
		adjustAllPoints(E.buf, topx, topy, botx, topy, 1);
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
		row = &E.buf->row[cur_row];

		if (row->size < botx) {
			memset(&rectBuf[idx * rw], ' ', rw);
			if (row->size > botx - rw) {
				int old_size = row->size;
				strncpy((char *)&rectBuf[idx * rw],
					(char *)&row->chars[botx - rw],
					row->size - (botx - rw));
				row->size -= (row->size - (botx - rw));
				row->chars[row->size] = 0;
				adjustAllPoints(E.buf, topx, cur_row, old_size,
						cur_row, 1);
			}
		} else {
			strncpy((char *)&rectBuf[idx * rw],
				(char *)&row->chars[botx - rw], rw);
			memcpy(&row->chars[topx], &row->chars[botx],
			       row->size - botx);
			row->size -= rw;
			row->chars[row->size] = 0;
			adjustAllPoints(E.buf, topx, cur_row, botx, cur_row, 1);
		}

		emil_strlcat((char *)new->data, (char *)row->chars,
			     new->datasize);
		idx++;
	}

	/* Finally, end line */
	if (topy != boty) {
		int cur_row = topy + idx;
		emil_strlcat((char *)new->data, "\n", new->datasize);
		row = &E.buf->row[cur_row];

		if (row->size < botx) {
			memset(&rectBuf[idx * rw], ' ', rw);
			if (row->size > botx - rw) {
				int old_size = row->size;
				strncpy((char *)&rectBuf[idx * rw],
					(char *)&row->chars[botx - rw],
					row->size - (botx - rw));
				row->size -= (row->size - (botx - rw));
				row->chars[row->size] = 0;
				adjustAllPoints(E.buf, topx, cur_row, old_size,
						cur_row, 1);
			}
		} else {
			strncpy((char *)&rectBuf[idx * rw],
				(char *)&row->chars[botx - rw], rw);
			memcpy(&row->chars[topx], &row->chars[botx],
			       row->size - botx);
			row->size -= rw;
			row->chars[row->size] = 0;
			adjustAllPoints(E.buf, topx, cur_row, botx, cur_row, 1);
		}

		undoAppendBounded(new, row->chars, row->size, topx);
	}
	new->datalen = strlen((char *)new->data);

	E.buf->dirty = 1;
	updateBuffer(E.buf);
	clearMarkQuiet();

	/* Phase 2: overwrite E.kill with rectangle data for the kill ring */
	/* TODO: copyRegion could take an output parameter to eliminate
	 * this two-phase use of E.kill */
	clearText(&E.kill);
	E.kill.str = rectBuf;
	E.kill.is_rectangle = 1;
	E.kill.rect_width = rw;
	E.kill.rect_height = rh;
	addToKillRing((char *)E.kill.str, 1, rw, rh);

	/* Restore the saved kill for non-rectangle use */
	clearText(&saved);
}

void yankRectangle(void) {
	int rw = E.kill.rect_width;
	int rh = E.kill.rect_height;

	struct text saved = E.kill;
	E.kill = (struct text){ 0 };

	int topx, topy, botx, boty;
	topx = E.buf->cx;
	topy = E.buf->cy;
	botx = topx;
	boty = topy + rh - 1;
	char *string = xcalloc(rw + 1, 1);

	/* Snapshot original row content BEFORE any mutation so that
	 * the delete undo record captures the true pre-edit state.
	 * This fixes: #16 (extra-line undo) and #17 (space-padding
	 * undo) — both were caused by capturing undo data after the
	 * buffer had been partially modified. */
	int orig_numrows = E.buf->numrows;
	int snap_count = (boty < orig_numrows) ? rh : orig_numrows - topy;
	int *snap_sizes = xmalloc(snap_count * sizeof(int));
	uint8_t **snap_chars = xmalloc(snap_count * sizeof(uint8_t *));
	for (int i = 0; i < snap_count; i++) {
		struct erow *r = &E.buf->row[topy + i];
		snap_sizes[i] = r->size;
		snap_chars[i] = xmalloc(r->size + 1);
		memcpy(snap_chars[i], r->chars, r->size + 1);
	}

	/* Add extra rows if the rectangle extends past the buffer */
	int extralines = 0;
	while (boty >= E.buf->numrows) {
		insertRow(E.buf, E.buf->numrows, "", 0);
		extralines++;
	}

	clearRedos(E.buf);

	/* Undo record 1 (bottom of stack): extra-lines insert.
	 * Records the newlines appended to extend the buffer. */
	if (extralines) {
		struct undo *u = newUndo();
		u->starty = orig_numrows - 1;
		u->startx = E.buf->row[u->starty].size;
		u->endx = 0;
		u->endy = E.buf->numrows - 1;
		if (extralines >= u->datasize) {
			u->datasize = extralines + 1;
			u->data = xrealloc(u->data, u->datasize);
		}
		memset(u->data, '\n', extralines);
		u->data[extralines] = 0;
		u->datalen = extralines;
		u->append = 0;
		u->delete = 0;
		pushUndo(E.buf, u);

		adjustAllPoints(E.buf, u->startx, u->starty, u->endx, u->endy,
				0);
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

	struct undo *del_undo = newUndo();
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
	pushUndo(E.buf, del_undo);

	/* Undo record 3 (top of stack): insert — will be filled with
	 * the post-mutation row content during the per-row loop. */
	struct undo *ins_undo = newUndo();
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
	pushUndo(E.buf, ins_undo);

	/* Free snapshot */
	for (int i = 0; i < snap_count; i++)
		free(snap_chars[i]);
	free(snap_chars);
	free(snap_sizes);

	/* Per-row rectangle insertion */
	for (int idx = 0; idx < rh; idx++) {
		int cur_row = topy + idx;
		struct erow *row = &E.buf->row[cur_row];

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
			adjustAllPoints(E.buf, topx, cur_row, topx + rw,
					cur_row, 0);

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
	ins_undo->endx = E.buf->row[boty].size;

	free(string);

	E.buf->dirty = 1;
	updateBuffer(E.buf);
	clearMarkQuiet();
	clearText(&E.kill);
	E.kill = saved;
}
