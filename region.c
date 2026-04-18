#include "region.h"
#include "adjust.h"
#include "buffer.h"
#include "dbuf.h"
#include "display.h"
#include "emil.h"
#include "history.h"
#include "message.h"
#include "mutate.h"
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

	/* Recheck the memory budget; sets E.memory_over_limit if the
	 * kill ring has pushed us over.  The status bar reads this
	 * flag and displays the persistent [MEMORY OVER!] warning. */
	recheckMemoryBudget();
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
	clampToBuffer(E.buf, &E.buf->markx, &E.buf->marky);
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
	clampToBuffer(E.buf, &E.buf->markx, &E.buf->marky);
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
	clampToBuffer(E.buf, &E.buf->markx, &E.buf->marky);
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

	int old_len;
	uint8_t *old_text =
		collectRegionText(E.buf, startx, starty, endx, endy, &old_len);

	/* Kill ring */
	if (add_to_kill_ring) {
		clearText(&E.kill);
		E.kill.str = (uint8_t *)xstrdup((char *)old_text);
		addToKillRing((char *)old_text, 0, 0, 0);
	}

	mutateDelete(E.buf, startx, starty, endx, endy, old_text, old_len);
	free(old_text);

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

	int len;
	uint8_t *text = collectRegionText(E.buf, E.buf->cx, E.buf->cy,
					  E.buf->markx, E.buf->marky, &len);
	clearText(&E.kill);
	E.kill.str = text;
	addToKillRing((char *)text, 0, 0, 0);

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

	int ex, ey;
	mutateInsert(E.buf, E.buf->cx, E.buf->cy, E.kill.str, killLen, &ex,
		     &ey);

	E.buf->cx = ex;
	E.buf->cy = ey;

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

	int old_len;
	uint8_t *old_text =
		collectRegionText(E.buf, startx, starty, endx, endy, &old_len);

	uint8_t *transformed = transformer(old_text);
	int repl_len = strlen((char *)transformed);

	int ex, ey;
	mutateReplace(E.buf, startx, starty, endx, endy, old_text, old_len,
		      transformed, repl_len, &ex, &ey);

	E.buf->cx = ex;
	E.buf->cy = ey;

	free(old_text);
	free(transformed);
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
	struct buffer *buf = E.buf;

	uint8_t *regex =
		editorPrompt(buf, "Regex replace: %s", PROMPT_BASIC, NULL);
	if (regex == NULL) {
		setStatusMessage(cancel);
		return;
	}

	char prompt[64];
	snprintf(prompt, sizeof(prompt), "Regex replace %.35s with: %%s",
		 regex);
	uint8_t *repl =
		editorPrompt(buf, (uint8_t *)prompt, PROMPT_BASIC, NULL);
	if (repl == NULL) {
		free(regex);
		setStatusMessage(cancel);
		return;
	}
	int replen = strlen((char *)repl);

	regex_t pattern;
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

	/* Collect old region text */
	int old_len;
	uint8_t *old_text = collectRegionText(buf, buf->cx, buf->cy, buf->markx,
					      buf->marky, &old_len);

	/* Build replacement text line by line */
	struct dbuf d = DBUF_INIT;
	int made = 0;

	for (int i = buf->cy; i <= buf->marky; i++) {
		erow *row = &buf->row[i];
		regmatch_t m[1];
		int matched =
			(regexec(&pattern, (char *)row->chars, 1, m, 0) == 0);

		/* Region slice boundaries for this row */
		int rstart = (i == buf->cy) ? buf->cx : 0;
		int rend = (i == buf->marky) ? buf->markx : row->size;

		/* Check match is within region bounds */
		int do_replace = 0;
		int mstart = 0, mlen = 0;
		if (matched) {
			mstart = m[0].rm_so;
			mlen = m[0].rm_eo - m[0].rm_so;
			if (mstart >= rstart && mstart + mlen <= rend)
				do_replace = 1;
		}

		if (i > buf->cy)
			dbuf_byte(&d, '\n');

		if (!do_replace) {
			dbuf_append(&d, &row->chars[rstart], rend - rstart);
		} else {
			made++;
			int pre = mstart - rstart;
			int post_src = mstart + mlen;
			int post_n = rend - post_src;
			dbuf_append(&d, &row->chars[rstart], pre);
			dbuf_append(&d, repl, replen);
			dbuf_append(&d, &row->chars[post_src], post_n);
		}
	}
	int out_len;
	uint8_t *out = dbuf_detach(&d, &out_len);

	uint8_t *okill = saveKill();

	int ex, ey;
	mutateReplace(buf, buf->cx, buf->cy, buf->markx, buf->marky, old_text,
		      old_len, out, out_len, &ex, &ey);

	buf->cx = ex;
	buf->cy = ey;

	free(old_text);
	free(out);
	regfree(&pattern);
	free(regex);
	free(repl);
	restoreKill(okill);
	setStatusMessage(msg_replaced_n, made);
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

	struct buffer *buf = E.buf;
	int slen = strlen((char *)string);
	int topx, topy, botx, boty;
	normalizeRectCols(&topx, &topy, &botx, &boty);

	/* Use full-row region so replacement text captures all content */
	int old_len;
	int region_endx = buf->row[boty].size;
	uint8_t *old_text =
		collectRegionText(buf, 0, topy, region_endx, boty, &old_len);

	/* Build replacement text: for each row, replace columns [topx..botx)
	 * with 'string', padding short rows with spaces as needed. */
	struct dbuf d = DBUF_INIT;

	for (int i = topy; i <= boty; i++) {
		erow *row = &buf->row[i];
		if (i > topy)
			dbuf_byte(&d, '\n');

		/* Pre-rectangle portion [0..topx) */
		if (topx > 0) {
			int avail = row->size;
			int copy_n = (topx < avail) ? topx : avail;
			if (copy_n > 0)
				dbuf_append(&d, row->chars, copy_n);
			/* Pad if row shorter than topx */
			int pad = topx - copy_n;
			if (pad > 0)
				dbuf_pad(&d, ' ', pad);
		}

		/* The replacement string */
		dbuf_append(&d, string, slen);

		/* Post-rectangle portion [botx..row->size) */
		int eff_botx = (botx > row->size) ? row->size : botx;
		if (eff_botx < row->size)
			dbuf_append(&d, &row->chars[eff_botx],
				    row->size - eff_botx);
	}
	int out_len;
	uint8_t *out = dbuf_detach(&d, &out_len);

	int ex, ey;
	mutateReplace(buf, 0, topy, region_endx, boty, old_text, old_len, out,
		      out_len, &ex, &ey);

	buf->cx = topx;
	buf->cy = topy;

	free(old_text);
	free(out);
	free(string);
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
	E.kill.str = xcalloc((size_t)rw * rh + 1, 1);
	E.kill.is_rectangle = 1;
	E.kill.rect_width = rw;
	E.kill.rect_height = rh;

	for (int idx = 0; idx < rh; idx++) {
		struct erow *row = &E.buf->row[topy + idx];
		if (row->size < botx) {
			memset(&E.kill.str[idx * rw], ' ', rw);
			if (row->size > topx) {
				int avail = row->size - topx;
				if (avail > rw)
					avail = rw;
				memcpy(&E.kill.str[idx * rw], &row->chars[topx],
				       avail);
			}
		} else {
			memcpy(&E.kill.str[idx * rw], &row->chars[topx], rw);
		}
	}

	addToKillRing((char *)E.kill.str, 1, rw, rh);
	clearMarkQuiet();
}

void killRectangle(void) {
	if (markInvalid())
		return;
	normalizeRegion();

	struct text saved = E.kill;
	E.kill = (struct text){ 0 };

	struct buffer *buf = E.buf;
	int topx, topy, botx, boty;
	normalizeRectCols(&topx, &topy, &botx, &boty);
	int rw = botx - topx;
	int rh = (boty - topy) + 1;

	/* Collect linear region text for undo */
	int old_len;
	/* Use full-row region so replacement text includes all content */
	int region_endx = buf->row[boty].size;
	uint8_t *old_text =
		collectRegionText(buf, 0, topy, region_endx, boty, &old_len);

	/* Extract rectangle columns into flat buffer for kill ring */
	uint8_t *rectBuf = xcalloc((size_t)rw * rh + 1, 1);
	for (int idx = 0; idx < rh; idx++) {
		erow *row = &buf->row[topy + idx];
		if (row->size < botx) {
			memset(&rectBuf[idx * rw], ' ', rw);
			if (row->size > topx) {
				int avail = row->size - topx;
				if (avail > rw)
					avail = rw;
				memcpy(&rectBuf[idx * rw], &row->chars[topx],
				       avail);
			}
		} else {
			memcpy(&rectBuf[idx * rw], &row->chars[topx], rw);
		}
	}

	/* Build post-deletion text: each row with columns [topx..botx)
	 * removed.  Full rows — matching the full-row region. */
	struct dbuf d = DBUF_INIT;

	for (int i = topy; i <= boty; i++) {
		erow *row = &buf->row[i];
		if (i > topy)
			dbuf_byte(&d, '\n');

		/* Portion before rectangle */
		if (topx > 0) {
			int n = (topx > row->size) ? row->size : topx;
			if (n > 0)
				dbuf_append(&d, row->chars, n);
		}
		/* Portion after rectangle */
		int eff_botx = (botx > row->size) ? row->size : botx;
		if (eff_botx < row->size)
			dbuf_append(&d, &row->chars[eff_botx],
				    row->size - eff_botx);
	}
	int out_len;
	uint8_t *out = dbuf_detach(&d, &out_len);

	int ex, ey;
	mutateReplace(buf, 0, topy, region_endx, boty, old_text, old_len, out,
		      out_len, &ex, &ey);

	buf->cx = topx;
	buf->cy = topy;

	/* Kill ring: rectangle data */
	clearText(&E.kill);
	E.kill.str = rectBuf;
	E.kill.is_rectangle = 1;
	E.kill.rect_width = rw;
	E.kill.rect_height = rh;
	addToKillRing((char *)E.kill.str, 1, rw, rh);

	free(old_text);
	free(out);
	clearMarkQuiet();
	clearText(&saved);
}

void yankRectangle(void) {
	int rw = E.kill.rect_width;
	int rh = E.kill.rect_height;

	struct text saved = E.kill;
	E.kill = (struct text){ 0 };

	struct buffer *buf = E.buf;
	int topx = buf->cx;
	int topy = buf->cy;
	int boty = topy + rh - 1;

	/* Extend buffer if rectangle goes past end */
	int extralines = 0;
	int orig_numrows = buf->numrows;
	while (boty >= buf->numrows) {
		insertRow(buf, buf->numrows, "", 0);
		extralines++;
	}

	clearRedos(buf);

	/* If we added rows, record a paired undo for the extension */
	if (extralines) {
		struct undo *ext = newUndo();
		ext->starty = orig_numrows - 1;
		ext->startx = buf->row[ext->starty].size;
		ext->endx = 0;
		ext->endy = buf->numrows - 1;
		if (extralines >= ext->datasize) {
			ext->datasize = extralines + 1;
			ext->data = xrealloc(ext->data, ext->datasize);
		}
		memset(ext->data, '\n', extralines);
		ext->data[extralines] = 0;
		ext->datalen = extralines;
		ext->append = 0;
		ext->delete = 0;
		pushUndo(buf, ext);
		adjustAllPoints(buf, ext->startx, ext->starty, ext->endx,
				ext->endy, 0);
	}

	/* Collect old text for the region [0,topy]..[eol,boty] */
	int old_len;
	uint8_t *old_text = collectRegionText(buf, 0, topy, buf->row[boty].size,
					      boty, &old_len);

	/* Build new text: for each row, insert rectangle slice at topx */
	struct dbuf d = DBUF_INIT;
	char *slice = xcalloc(rw + 1, 1);

	for (int idx = 0; idx < rh; idx++) {
		int cur = topy + idx;
		erow *row = &buf->row[cur];
		if (idx > 0)
			dbuf_byte(&d, '\n');

		strncpy(slice, (char *)&saved.str[idx * rw], rw);

		/* Row content before topx (with space padding) */
		int pre_len = (row->size < topx) ? row->size : topx;
		dbuf_append(&d, row->chars, pre_len);
		int pad = topx - pre_len;
		if (pad > 0)
			dbuf_pad(&d, ' ', pad);

		/* Rectangle slice */
		dbuf_append(&d, (uint8_t *)slice, rw);

		/* Remainder of row after topx */
		if (pre_len == topx && topx < row->size)
			dbuf_append(&d, &row->chars[topx], row->size - topx);
	}
	int pos;
	uint8_t *out = dbuf_detach(&d, &pos);
	free(slice);

	/* Undo record: delete old content (paired with extension if any) */
	struct undo *del = newUndo();
	del->startx = 0;
	del->starty = topy;
	del->endx = buf->row[boty].size;
	del->endy = boty;
	del->delete = 1;
	del->append = 0;
	del->paired = extralines ? 1 : 0;
	undoReplaceData(del, old_len + 1);
	memcpy(del->data, old_text, old_len);
	del->data[old_len] = 0;
	del->datalen = old_len;
	pushUndo(buf, del);

	/* Perform deletion */
	bulkDelete(buf, 0, topy, buf->row[boty].size, boty);

	/* Compute insert end position */
	int iex = 0, iey = topy;
	for (int i = 0; i < pos; i++) {
		if (out[i] == '\n') {
			iey++;
			iex = 0;
		} else {
			iex++;
		}
	}

	/* Undo record: insert new content */
	struct undo *ins = newUndo();
	ins->startx = 0;
	ins->starty = topy;
	ins->endx = iex;
	ins->endy = iey;
	ins->delete = 0;
	ins->append = 0;
	ins->paired = 1;
	undoReplaceData(ins, pos + 1);
	memcpy(ins->data, out, pos);
	ins->data[pos] = 0;
	ins->datalen = pos;
	pushUndo(buf, ins);

	/* Perform insertion */
	bulkInsert(buf, 0, topy, out, pos);

	buf->cx = topx;
	buf->cy = topy;
	markBufferDirty(buf);
	updateBuffer(buf);

	free(old_text);
	free(out);
	clearMarkQuiet();
	clearText(&E.kill);
	E.kill = saved;
}
