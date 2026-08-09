/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include "region.h"
#include "adjust.h"
#include "buffer.h"
#include "dbuf.h"
#include "display.h"
#include "emil.h"
#include "history.h"

#include "mutate.h"
#include "prompt.h"
#include "undo.h"
#include "unicode.h"
#include "util.h"
#include <regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
static struct text saveKill(void) {
	struct text saved = E.kill;
	if (saved.str != NULL)
		saved.str = (uint8_t *)xstrdup((char *)saved.str);
	return saved;
}

static void restoreKill(struct text saved) {
	free(E.kill.str);
	E.kill = saved;
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
		setStatusMessage("Mark deactivated");
		return;
	}
	markRingPush();
	E.buf->markx = E.buf->cx;
	E.buf->marky = E.buf->cy;
	E.buf->mark_active = 1;
	setStatusMessage("Mark set.");
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
		setStatusMessage("No mark set in this buffer.");
		return;
	}

	int old_cx = E.buf->cx;
	int old_cy = E.buf->cy;

	E.buf->cx = buf->markx;
	E.buf->cy = buf->marky;

	/* Clamp */
	if (E.buf->cy >= E.buf->numrows)
		buf->cy = E.buf->numrows - 1;
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

		/* Backstop: adjustAllPoints() keeps ring entries live
		 * across mutations, but an entry can also predate a
		 * buffer reload, so snap what we restore rather than
		 * trusting it.  Bounds first -- a byte offset must be
		 * inside the row before it can be tested for being
		 * mid-character. */
		if (buf->marky >= buf->numrows)
			buf->marky = buf->numrows - 1;
		if (buf->marky < buf->numrows) {
			erow *mrow = &buf->row[buf->marky];
			if (buf->markx > mrow->size)
				buf->markx = mrow->size;
			while (buf->markx > 0 &&
			       utf8_isCont(mrow->chars[buf->markx]))
				buf->markx--;
		} else {
			buf->markx = 0;
		}
	}

	E.buf->mark_active = 0;

	if (E.buf->cx == old_cx && E.buf->cy == old_cy)
		setStatusMessage("Mark popped.");
}

void toggleRectangleMode(void) {
	E.buf->rectangle_mode = !E.buf->rectangle_mode;
	if (E.buf->rectangle_mode) {
		setStatusMessage("Rectangle mode ON");
	} else {
		setStatusMessage("Rectangle mode OFF");
	}
}

void markBuffer(void) {
	if (!bufferIsEmpty(E.buf)) {
		E.buf->cy = E.buf->numrows - 1;
		E.buf->cx = E.buf->row[E.buf->cy].size;
		setMark();
		E.buf->cy = 0;
		E.buf->cx = 0;
	}
}

/* Validity of a specific buffer's mark.  Callers that render or edit a
 * buffer other than the focused one must use this form: the no-arg
 * wrapper below consults the global E.buf, which is only the right
 * question when the buffer in hand IS E.buf. */
int markInvalidBuf(const struct buffer *buf) {
	return (buf->markx < 0 || buf->marky < 0 ||
		buf->marky >= buf->numrows ||
		buf->markx > (buf->row[buf->marky].size) ||
		(buf->markx == buf->cx && buf->cy == buf->marky));
}

int markInvalidSilent(void) {
	return markInvalidBuf(E.buf);
}

int markInvalid(void) {
	int ret = markInvalidSilent();

	if (ret) {
		setStatusMessage("Mark invalid.");
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

/* Rectangle columns are byte offsets.  The point and mark always sit
 * on character boundaries in their own rows, but the same byte
 * offsets applied to the rows in between (or to each other's rows)
 * can land inside a multi-byte UTF-8 sequence.  Snap such offsets to
 * character boundaries, inward: a character belongs to the rectangle
 * only if it lies entirely within [topx..botx) on its own row.
 * Partially covered characters are left whole in the buffer and
 * excluded from the extracted rectangle.  This preserves the
 * invariant that every buffer (and the kill ring) contains only
 * valid UTF-8. */

/* Smallest character boundary >= x.  Offsets beyond the row are
 * returned unchanged so callers' short-row padding logic still sees
 * the nominal column. */
static int rectSnapFwd(erow *row, int x) {
	if (x <= 0)
		return 0;
	if (x >= row->size)
		return x;
	while (x < row->size && utf8_isCont(row->chars[x]))
		x++;
	return x;
}

/* Largest character boundary <= x, clamped to the row.  chars[size]
 * is the NUL terminator, never a continuation byte, so x == size is
 * already a boundary. */
static int rectSnapBack(erow *row, int x) {
	if (x <= 0)
		return 0;
	if (x > row->size)
		return row->size;
	while (x > 0 && utf8_isCont(row->chars[x]))
		x--;
	return x;
}

/* Place the cursor at byte column x in row y, snapped forward to a
 * character boundary and clamped to the row. */
static void rectPlaceCursor(struct buffer *buf, int x, int y) {
	buf->cy = y;
	buf->cx = rectSnapFwd(&buf->row[y], x);
	if (buf->cx > buf->row[y].size)
		buf->cx = buf->row[y].size;
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
	/* topx comes from whichever of point/mark had the smaller
	 * column, which need not be a character boundary in row topy
	 * (and likewise botx in row boty), so snap the repositioned
	 * point and mark onto boundaries.  topx/botx themselves stay
	 * nominal: per-row snapping happens where rows are sliced. */
	E.buf->cx = rectSnapFwd(&E.buf->row[*topy], *topx);
	if (E.buf->cx > E.buf->row[*topy].size)
		E.buf->cx = E.buf->row[*topy].size;
	E.buf->cy = *topy;
	E.buf->marky = *boty;
	E.buf->markx = rectSnapBack(&E.buf->row[*boty], *botx);
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
	if (rejectIfReadOnly(E.buf))
		return;

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

/* Direction of the most recent user-level yank.  M-y cycling repeats
 * the insertion in the same style, so "C-u C-y M-y M-y ..." keeps
 * point before each candidate just as the initial yank did. */
static int yank_style_reverse = 0;

/* Insert the current kill at point.  Forward style leaves point after
 * the inserted text; reverse style leaves point before it and sets
 * the mark after it.  Does not touch E.kill_ring_pos — that is the
 * caller's bookkeeping. */
static void yankInsert(int reverse) {
	if (E.kill.is_rectangle) {
		/* Rectangles have their own geometry-driven point
		 * placement; the reverse style does not apply. */
		yankRectangle();
		return;
	}

	int killLen = strlen((char *)E.kill.str);

	int sx = E.buf->cx, sy = E.buf->cy;
	int ex, ey;
	mutateInsert(E.buf, sx, sy, E.kill.str, killLen, &ex, &ey);

	if (reverse) {
		E.buf->cx = sx;
		E.buf->cy = sy;
		E.buf->markx = ex;
		E.buf->marky = ey;
		E.buf->mark_active = 0;
	} else {
		E.buf->cx = ex;
		E.buf->cy = ey;
	}
}

void yank(int uarg) {
	/* M-- C-y is undefined by design: the reverse modifier belongs
	 * to M-y, transpose, and the case commands. */
	if (uarg == UARG_REVERSE)
		return;

	if (rejectIfReadOnly(E.buf))
		return;

	if (E.kill.str == NULL) {
		setStatusMessage("Kill ring empty.");
		return;
	}

	/* Any C-u prefix (there is no numeric meaning for yank) selects
	 * the reverse style: point stays before the yanked text, mark
	 * is set after it. */
	yank_style_reverse = uarg > 0;
	yankInsert(yank_style_reverse);

	/* Set kill ring position so M-y continues from here */
	E.kill_ring_pos = E.kill_history.count > 0 ? E.kill_history.count - 1 :
						     0;
}

void yankPop(int uarg) {
	/* C-u M-y is undefined by design: a repeat count has no meaning
	 * for cycling, and numeric ring selection does not exist. */
	if (uarg > 0)
		return;

	if (rejectIfReadOnly(E.buf))
		return;

	if (E.kill_history.count == 0) {
		setStatusMessage("Kill ring empty.");
		return;
	}

	if (E.kill_ring_pos < 0) {
		setStatusMessage("Previous command was not a yank");
		return;
	}

	/* E.kill_ring_pos is emil's last-command flag: processKeypress
		 * resets it to -1 for every command except yank, yank-pop and
		 * the argument keys.  A non-negative value already means the
		 * preceding user-level command was a yank or yank-pop.
		 *
		 * What is still needed is the assurance that there is a record
		 * to undo.  The prompt loop dispatches C-y through
		 * processKeypress but handles history browsing itself, so
		 * "C-y, Up, M-y" in a prompt leaves kill_ring_pos set while
		 * replaceMinibufferText has cleared the undo stack. */
	if (E.buf->undo == NULL) {
		setStatusMessage("Previous command was not a yank");
		return;
	}

	doUndo(E.buf, 1);

	if (uarg == UARG_REVERSE) {
		/* M-- M-y: cycle toward newer kills. */
		E.kill_ring_pos++;
		if (E.kill_ring_pos >= E.kill_history.count)
			E.kill_ring_pos = 0;
	} else {
		E.kill_ring_pos--;
		if (E.kill_ring_pos < 0)
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
		yankInsert(yank_style_reverse);
	} else {
		setStatusMessage("No more kill ring entries to yank!");
	}
}

void transformRange(int startx, int starty, int endx, int endy,
		    uint8_t *(*transformer)(uint8_t *)) {
	if (rejectIfReadOnly(E.buf))
		return;

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
	/* A transformer may fail (transformerPipeCmd returns NULL when
	 * the subprocess can't be spawned or joined).  Leave the
	 * buffer untouched. */
	if (transformed == NULL) {
		free(old_text);
		return;
	}
	int repl_len = strlen((char *)transformed);

	int ex, ey;
	mutateReplace(E.buf, startx, starty, endx, endy, old_text, old_len,
		      transformed, repl_len, 0, &ex, &ey);

	E.buf->cx = ex;
	E.buf->cy = ey;

	free(old_text);
	free(transformed);
}

void transformRegion(uint8_t *(*transformer)(uint8_t *)) {
	if (rejectIfReadOnly(E.buf))
		return;

	if (markInvalid())
		return;
	normalizeRegion();

	transformRange(E.buf->cx, E.buf->cy, E.buf->markx, E.buf->marky,
		       transformer);
}

/* Replacement-template escapes.  Only \& (the whole match), \1-\9
 * (capture groups) and \\ (a literal backslash) are recognised;
 * every other escape is an error, as in Emacs, which signals
 * "Invalid use of `\' in replacement text".
 *
 * Returns NULL when 'tmpl' is well formed, else a static string
 * naming the fault.  'nsub' is the pattern's capture-group count
 * (regex_t.re_nsub); a reference past it gets rejected.
 *
 * Checked up front, before any match is attempted, so a bad
 * template is reported even when the pattern matches nothing.
 * Exposed for tests/test_replace.c. */
const char *replacementTemplateError(const uint8_t *tmpl, size_t nsub) {
	for (int i = 0; tmpl[i]; i++) {
		if (tmpl[i] != '\\')
			continue;
		uint8_t c = tmpl[i + 1];
		if (c == '\0')
			return "Trailing backslash in replacement";
		if (c == '\\' || c == '&') {
			i++;
			continue;
		}
		if (c >= '1' && c <= '9') {
			if ((size_t)(c - '0') > nsub)
				return "Replacement refers to nonexistent group";
			i++;
			continue;
		}
		return "Invalid use of backslash in replacement";
	}
	return NULL;
}

/* Expand 'tmpl' for a single match into 'out'.  'm' holds offsets
 * absolute to 'subject'.  replacementTemplateError has already
 * passed, so nothing is validated here. */
static void expandTemplate(struct dbuf *out, const uint8_t *tmpl,
			   const uint8_t *subject, const regmatch_t *m) {
	for (int i = 0; tmpl[i]; i++) {
		if (tmpl[i] != '\\') {
			dbuf_byte(out, tmpl[i]);
			continue;
		}
		uint8_t c = tmpl[++i];
		if (c == '\\') {
			dbuf_byte(out, '\\');
			continue;
		}
		int g = (c == '&') ? 0 : c - '0';
		/* A group that did not participate in the match
		 * contributes nothing, as in Emacs. */
		if (m[g].rm_so >= 0)
			dbuf_append(out, subject + m[g].rm_so,
				    (int)(m[g].rm_eo - m[g].rm_so));
	}
}

/* Substitute every match of 're' in 'subject' (NUL-terminated,
 * 'len' bytes) using template 'tmpl'.
 *
 * The caller compiles with REG_NEWLINE, so ^ and $ anchor at
 * embedded newlines and . does not cross one.  'notbol' / 'noteol'
 * cover the ends: a region starting mid-line must not let ^ match
 * at its first byte, nor one ending mid-line let $ match at its
 * last.
 *
 * Returns the match count; on 0 the out-params are untouched.
 * Exposed for tests/test_replace.c. */
int regexSubstituteAll(const regex_t *re, const uint8_t *subject, int len,
		       const uint8_t *tmpl, int notbol, int noteol,
		       struct dbuf *out, int *first_off, int *last_off) {
	size_t nmatch = re->re_nsub + 1;
	regmatch_t *m = xmalloc(nmatch * sizeof(*m));
	regmatch_t *abs = xmalloc(nmatch * sizeof(*abs));
	int pos = 0, count = 0, first = 0, copied = 0, prev_end = -1;

	while (pos <= len) {
		int eflags = noteol ? REG_NOTEOL : 0;
		/* Restarting mid-subject makes regexec see a fresh
		 * string, so ^ would match at the restart point.
		 * Suppress it unless the restart really does follow a
		 * newline. */
		if (pos == 0 ? notbol : subject[pos - 1] != '\n')
			eflags |= REG_NOTBOL;

		if (regexec(re, (const char *)subject + pos, nmatch, m,
			    eflags) != 0)
			break;

		int so = pos + (int)m[0].rm_so;
		int eo = pos + (int)m[0].rm_eo;

		for (size_t g = 0; g < nmatch; g++) {
			abs[g].rm_so = m[g].rm_so < 0 ? -1 : pos + m[g].rm_so;
			abs[g].rm_eo = m[g].rm_eo < 0 ? -1 : pos + m[g].rm_eo;
		}

		/* An empty match butted against the end of the previous
		 * match is not a new occurrence. */
		int adjacent_empty = (eo == so && so == prev_end);

		if (!adjacent_empty) {
			if (count == 0)
				first = copied = so;
			else
				dbuf_append(out, subject + copied, so - copied);

			expandTemplate(out, tmpl, subject, abs);
			copied = eo;
			prev_end = eo;
			count++;
		}

		if (eo != so) {
			pos = eo;
			continue;
		}

		if (so >= len)
			break;
		int n = utf8_nBytes(subject[so]);
		if (n < 1 || so + n > len)
			n = 1;
		dbuf_append(out, subject + copied, so - copied);
		dbuf_append(out, subject + so, n);
		copied = pos = so + n;
	}

	free(m);
	free(abs);

	if (count > 0) {
		*first_off = first;
		*last_off = copied;
	}
	return count;
}

/* Map a byte offset within region text back to buffer coordinates.
 * The region string joins rows with '\n' (see collectRegionText), so
 * walking it reproduces the row/column the offset came from. */
static void offsetToCoords(int startx, int starty, const uint8_t *s, int off,
			   int *x, int *y) {
	int cx = startx, cy = starty;
	for (int i = 0; i < off; i++) {
		if (s[i] == '\n') {
			cy++;
			cx = 0;
		} else {
			cx++;
		}
	}
	*x = cx;
	*y = cy;
}

void replaceRegex(void) {
	if (rejectIfReadOnly(E.buf))
		return;

	struct buffer *buf = E.buf;
	if (bufferIsEmpty(buf)) {
		setStatusMessage("Buffer is empty.");
		return;
	}

	/* Emacs scoping: the region when it is active, otherwise from
	 * point to end of buffer. */
	int use_region = buf->mark_active && !markInvalidSilent();
	if (use_region)
		normalizeRegion();

	int startx = buf->cx, starty = buf->cy, endx, endy;
	if (use_region) {
		endx = buf->markx;
		endy = buf->marky;
	} else {
		endy = buf->numrows - 1;
		endx = buf->row[endy].size;
	}

	if (starty > endy || (starty == endy && startx >= endx)) {
		setStatusMessage("Nothing to replace.");
		return;
	}

	const char *cancel = "Canceled regex-replace.";

	uint8_t *regex =
		editorPrompt(buf, "Regex replace: ", PROMPT_REPLACE, NULL);
	if (regex == NULL) {
		setStatusMessage("%s", cancel);
		return;
	}

	/* Cap the displayed pattern to 35 chars.  The prompt is a plain
	 * prefix (see editorPrompt), so no percent escaping is needed
	 * and %.35s truncation is safe.  A literal newline in the
	 * pattern is shown as ^J; embedded raw it would reach the
	 * terminal as a line feed and split the minibuffer. */
	char *esc = caretEscapeNewlines(regex);
	char prompt[128];
	snprintf(prompt, sizeof(prompt), "Regex replace %.35s with: ", esc);
	free(esc);
	uint8_t *repl = editorPrompt(buf, prompt, PROMPT_REPLACE, NULL);
	if (repl == NULL) {
		free(regex);
		setStatusMessage("%s", cancel);
		return;
	}

	/* REG_NEWLINE keeps ^ and $ anchored to line boundaries and
	 * stops . crossing a newline, so matching the region as one
	 * string still behaves per-line. */
	regex_t pattern;
	int rc = regcomp(&pattern, (char *)regex, REG_EXTENDED | REG_NEWLINE);
	if (rc != 0) {
		char error_msg[256];
		regerror(rc, &pattern, error_msg, sizeof(error_msg));
		setStatusMessage("Regex error: %s", error_msg);
		free(regex);
		free(repl);
		return;
	}

	const char *terr = replacementTemplateError(repl, pattern.re_nsub);
	if (terr != NULL) {
		setStatusMessage("%s", terr);
		regfree(&pattern);
		free(regex);
		free(repl);
		return;
	}

	int old_len;
	uint8_t *old_text =
		collectRegionText(buf, startx, starty, endx, endy, &old_len);

	struct dbuf d = DBUF_INIT;
	int first_off = 0, last_off = 0;
	int made = regexSubstituteAll(&pattern, old_text, old_len, repl,
				      startx > 0, endx < buf->row[endy].size,
				      &d, &first_off, &last_off);

	if (made == 0) {
		/* No mutateReplace: the predecessor rewrote the region
		 * with itself even at zero matches, which left a
		 * pointless undo record and a dirty flag behind. */
		dbuf_free(&d);
		free(old_text);
		regfree(&pattern);
		free(regex);
		free(repl);
		setStatusMessage("Replaced 0 occurrences");
		return;
	}

	int out_len;
	uint8_t *out = dbuf_detach(&d, &out_len);

	int fx, fy, lx, ly;
	offsetToCoords(startx, starty, old_text, first_off, &fx, &fy);
	offsetToCoords(startx, starty, old_text, last_off, &lx, &ly);

	int ex, ey;
	mutateReplace(buf, fx, fy, lx, ly, old_text + first_off,
		      last_off - first_off, out, out_len, 0, &ex, &ey);

	buf->cx = ex;
	buf->cy = ey;

	free(old_text);
	free(out);
	regfree(&pattern);
	free(regex);
	free(repl);
	setStatusMessage("Replaced %d occurrences", made);
}

void stringRectangle(void) {
	if (rejectIfReadOnly(E.buf))
		return;

	if (markInvalid())
		return;

	uint8_t *string =
		editorPrompt(E.buf, "String rectangle: ", PROMPT_RECT, NULL);
	if (string == NULL) {
		setStatusMessage("Canceled.");
		return;
	}

	stringRectangleWithText(string);
	free(string);
}

/* Core of stringRectangle, separated from the interactive prompt so
 * it can be exercised by the test suite.  Does not free 'string'. */
void stringRectangleWithText(uint8_t *string) {
	struct text okill = saveKill();
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

		/* Snap inward to character boundaries so no multi-byte
		 * character is split; partially covered characters
		 * stay whole outside the replacement. */
		int s = rectSnapFwd(row, topx);
		int e = rectSnapBack(row, botx);
		/* A rectangle whose columns both fall inside one
		 * multi-byte character snaps to an inverted pair: the
		 * left edge forward past the character, the right edge
		 * back before it.  That is a zero-width intersection
		 * with this row, so say so here rather than leaving
		 * each consumer to derive it from its own clamp. */
		if (e < s)
			e = s;

		/* Pre-rectangle portion [0..s) */
		int copy_n = (s < row->size) ? s : row->size;
		if (copy_n > 0)
			dbuf_append(&d, row->chars, copy_n);
		/* Pad if row shorter than the nominal column */
		int pad = topx - copy_n;
		if (pad > 0)
			dbuf_pad(&d, ' ', pad);

		/* The replacement string */
		dbuf_append(&d, string, slen);

		/* Post-rectangle portion [e..row->size) */
		if (e < copy_n)
			e = copy_n;
		if (e < row->size)
			dbuf_append(&d, &row->chars[e], row->size - e);
	}
	int out_len;
	uint8_t *out = dbuf_detach(&d, &out_len);

	int ex, ey;
	mutateReplace(buf, 0, topy, region_endx, boty, old_text, old_len, out,
		      out_len, 0, &ex, &ey);

	rectPlaceCursor(buf, topx, topy);

	free(old_text);
	free(out);
	E.buf->mark_active = 0;
	;
	restoreKill(okill);
}

/* Extract rectangle columns [topx..topx+rw) from rows [topy..topy+rh)
 * into a newly allocated flat buffer (rw bytes per row, space-padded
 * for short rows).  Caller frees. */
static uint8_t *extractRectColumns(struct buffer *buf, int topx, int topy,
				   int rw, int rh) {
	int botx = topx + rw;
	uint8_t *out = xcalloc((size_t)rw * rh + 1, 1);
	for (int idx = 0; idx < rh; idx++) {
		erow *row = &buf->row[topy + idx];
		memset(&out[idx * rw], ' ', rw);
		/* Snap inward so no multi-byte character is split; the
		 * slice can only shrink (s >= topx, e <= botx), so it
		 * always fits in the rw-byte cell, space-padded. */
		int s = rectSnapFwd(row, topx);
		int e = rectSnapBack(row, botx);
		/* A rectangle whose columns both fall inside one
		 * multi-byte character snaps to an inverted pair: the
		 * left edge forward past the character, the right edge
		 * back before it.  That is a zero-width intersection
		 * with this row, so say so here rather than leaving
		 * each consumer to derive it from its own clamp. */
		if (e < s)
			e = s;
		if (s >= row->size || e <= s)
			continue;
		memcpy(&out[idx * rw], &row->chars[s], e - s);
	}
	return out;
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
	E.kill.str = extractRectColumns(E.buf, topx, topy, rw, rh);
	E.kill.is_rectangle = 1;
	E.kill.rect_width = rw;
	E.kill.rect_height = rh;

	addToKillRing((char *)E.kill.str, 1, rw, rh);
	E.buf->mark_active = 0;
	;
}

void killRectangle(void) {
	if (rejectIfReadOnly(E.buf))
		return;

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
	uint8_t *rectBuf = extractRectColumns(buf, topx, topy, rw, rh);

	/* Build post-deletion text: each row with columns [topx..botx)
	 * removed.  Full rows — matching the full-row region. */
	struct dbuf d = DBUF_INIT;

	for (int i = topy; i <= boty; i++) {
		erow *row = &buf->row[i];
		if (i > topy)
			dbuf_byte(&d, '\n');

		/* Snap inward to character boundaries: what stays is
		 * exactly the complement of what extractRectColumns
		 * took, so partially covered characters remain whole
		 * in the buffer. */
		int s = rectSnapFwd(row, topx);
		int e = rectSnapBack(row, botx);
		/* A rectangle whose columns both fall inside one
		 * multi-byte character snaps to an inverted pair: the
		 * left edge forward past the character, the right edge
		 * back before it.  That is a zero-width intersection
		 * with this row, so say so here rather than leaving
		 * each consumer to derive it from its own clamp. */
		if (e < s)
			e = s;

		/* Portion before rectangle */
		int n = (s > row->size) ? row->size : s;
		if (n > 0)
			dbuf_append(&d, row->chars, n);
		/* Portion after rectangle */
		if (e < n)
			e = n;
		if (e < row->size)
			dbuf_append(&d, &row->chars[e], row->size - e);
	}
	int out_len;
	uint8_t *out = dbuf_detach(&d, &out_len);

	int ex, ey;
	mutateReplace(buf, 0, topy, region_endx, boty, old_text, old_len, out,
		      out_len, 0, &ex, &ey);

	rectPlaceCursor(buf, topx, topy);

	/* Kill ring: rectangle data */
	clearText(&E.kill);
	E.kill.str = rectBuf;
	E.kill.is_rectangle = 1;
	E.kill.rect_width = rw;
	E.kill.rect_height = rh;
	addToKillRing((char *)E.kill.str, 1, rw, rh);

	free(old_text);
	free(out);
	E.buf->mark_active = 0;
	;
	clearText(&saved);
}

void yankRectangle(void) {
	if (rejectIfReadOnly(E.buf))
		return;

	/* C-x r y is bound here unconditionally, so guard against a
	 * missing or non-rectangle kill: with rect_height == 0 the
	 * arithmetic below would compute boty = cy - 1 and read
	 * row[-1] at the top of the buffer. */
	if (E.kill.str == NULL || !E.kill.is_rectangle ||
	    E.kill.rect_width <= 0 || E.kill.rect_height <= 0) {
		setStatusMessage("Last kill is not a rectangle.");
		return;
	}

	int rw = E.kill.rect_width;
	int rh = E.kill.rect_height;

	struct text saved = E.kill;
	E.kill = (struct text){ 0 };

	struct buffer *buf = E.buf;
	int topx = buf->cx;
	int topy = buf->cy;
	int boty = topy + rh - 1;

	/* If the rectangle overruns the buffer, extend with blank
		 * rows first.  mutateExtendRows pushes a single pure-insert
		 * undo with paired=0 at the head of the chain; the following
		 * mutateReplace passes chain_to_prev = needs_extension so its
		 * delete record pairs back to the extension, and the pair's
		 * insert pairs to the delete -- three records, one atomic
		 * undo.
		 *
		 * The extension runs first, so by the time mutateReplace
		 * looks at (0, topy) that row is a real row, and the
		 * following bulkDelete only ever shrinks the buffer back to
		 * topy + 1 rows. */
	int needs_extension = 0;
	if (boty >= buf->numrows) {
		int n = boty - buf->numrows + 1;
		mutateExtendRows(buf, buf->numrows, n);
		needs_extension = 1;
	}

	/* Collect old text for the full-row region [0,topy]..[eol,boty] */
	int region_endx = buf->row[boty].size;
	int old_len;
	uint8_t *old_text =
		collectRegionText(buf, 0, topy, region_endx, boty, &old_len);

	/* Build new text: for each row, insert rectangle slice at topx,
	 * padding with spaces if the row is shorter than topx. */
	struct dbuf d = DBUF_INIT;
	char *slice = xcalloc(rw + 1, 1);

	for (int idx = 0; idx < rh; idx++) {
		int cur = topy + idx;
		erow *row = &buf->row[cur];
		if (idx > 0)
			dbuf_byte(&d, '\n');

		strncpy(slice, (char *)&saved.str[idx * rw], rw);

		/* Snap the per-row insertion point forward to a
		 * character boundary so a destination multi-byte
		 * character is never split by the insertion. */
		int ins = rectSnapFwd(row, topx);

		/* Row content before the insertion point (with space
		 * padding relative to the nominal column) */
		int pre_len = (row->size < ins) ? row->size : ins;
		dbuf_append(&d, row->chars, pre_len);
		int pad = topx - pre_len;
		if (pad > 0)
			dbuf_pad(&d, ' ', pad);

		/* Rectangle slice */
		dbuf_append(&d, (uint8_t *)slice, rw);

		/* Remainder of row after the insertion point */
		if (pre_len < row->size)
			dbuf_append(&d, &row->chars[pre_len],
				    row->size - pre_len);
	}
	int out_len;
	uint8_t *out = dbuf_detach(&d, &out_len);
	free(slice);

	mutateReplace(buf, 0, topy, region_endx, boty, old_text, old_len, out,
		      out_len, needs_extension, NULL, NULL);

	rectPlaceCursor(buf, topx, topy);

	free(old_text);
	free(out);
	E.buf->mark_active = 0;
	;
	clearText(&E.kill);
	E.kill = saved;
}
