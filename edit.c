/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include "edit.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "history.h"
#include "keymap.h"

#include "motion.h"
#include "dbuf.h"
#include "mutate.h"
#include "prompt.h"
#include "region.h"
#include "terminal.h"
#include "transform.h"
#include "undo.h"
#include "unicode.h"
#include "util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

/* Character insertion */

/* Raw insertion with no undo record.  Used for minibuffer text, which
 * is not part of any undo history.  Editing a user buffer goes through
 * selfInsert below, or the mutation layer directly. */
void insertChar(struct buffer *bufr, int c, int count) {
	if (rejectIfReadOnly(bufr))
		return;

	bufr->mark_active = 0;

	if (count <= 0)
		count = 1;

	for (int i = 0; i < count; i++) {
		/* No virtual row to materialise: cy < numrows (#105). */
		rowInsertChar(bufr, &bufr->row[bufr->cy], bufr->cx, c);
		bufr->cx++;
	}
}

/* Insert 'len' bytes of 'text' 'times' times at point, recording undo,
 * and leave point after the insertion.
 *
 * A single repetition may join the run at the head of the undo list —
 * that's what makes typing a word one undo step.  An explicit prefix
 * argument is one command, so it produces one record however many
 * copies it asks for, rather than a run the cap would then chop up. */
static void insertRepeat(struct buffer *buf, const uint8_t *text, int len,
			 int times) {
	int ex, ey;

	if (times == 1) {
		mutateInsertChar(buf, buf->cx, buf->cy, text, len, &ex, &ey);
	} else {
		struct dbuf d = DBUF_INIT;
		for (int i = 0; i < times; i++)
			dbuf_append(&d, text, len);
		mutateInsert(buf, buf->cx, buf->cy, d.buf, d.len, &ex, &ey);
		dbuf_free(&d);
	}
	buf->cx = ex;
	buf->cy = ey;
}

void insertUnicode(int count) {
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;
	insertRepeat(E.buf, E.unicode, E.nunicode, UARG_COUNT(count));
}

/* Insert 'c' 'count' times at point, recording undo.  This is the
 * undoable typing path; insertChar above is the raw primitive, used
 * for minibuffer text that is not part of the undo history. */
void selfInsert(struct buffer *bufr, int c, int count) {
	if (rejectIfReadOnly(bufr))
		return;

	bufr->mark_active = 0;

	if (count <= 0)
		count = 1;

	uint8_t byte = (uint8_t)c;
	insertRepeat(bufr, &byte, 1, count);
}

/* Line operations */

void insertNewline(int count) {
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;

	const uint8_t nl = '\n';
	insertRepeat(E.buf, &nl, 1, UARG_COUNT(count));
}

void openLine(int count) {
	if (count <= 0)
		count = 1;

	for (int i = 0; i < count; i++) {
		int ccx = E.buf->cx;
		int ccy = E.buf->cy;
		insertNewline(1);
		E.buf->cx = ccx;
		E.buf->cy = ccy;
	}
}

void insertNewlineAndIndent(int count) {
	if (rejectIfReadOnly(E.buf))
		return;

	if (count <= 0)
		count = 1;

	for (int j = 0; j < count; j++) {
		insertNewline(1);
		erow *prev = &E.buf->row[E.buf->cy - 1];
		int i = 0;
		while (i < prev->size &&
		       (prev->chars[i] == ' ' || prev->chars[i] == CTRL('i'))) {
			uint8_t c = prev->chars[i];
			int ex, ey;
			mutateInsertChar(E.buf, E.buf->cx, E.buf->cy, &c, 1,
					 &ex, &ey);
			E.buf->cx = ex;
			E.buf->cy = ey;
			prev = &E.buf->row[E.buf->cy - 1];
			i++;
		}
	}
}

/* Indentation */

void unindent(int rept) {
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;

	struct erow *row = &E.buf->row[E.buf->cy];

	/* Calculate size of unindent */
	/* NB: trunc is bounded by the NUL terminator at chars[size],
	 * which always mismatches '\t'. */
	int times = UARG_COUNT(rept);
	int trunc = 0;
	for (int i = 0; i < times; i++) {
		if (row->chars[trunc] != '\t')
			break;
		trunc++;
	}

	if (trunc == 0)
		return;

	/* Build old_text for mutateDelete */
	uint8_t *old_text = xmalloc(trunc + 1);
	memset(old_text, '\t', trunc);
	old_text[trunc] = 0;

	mutateDelete(E.buf, 0, E.buf->cy, trunc, E.buf->cy, old_text, trunc);
	free(old_text);

	/* If the cursor sat inside the removed indentation (cx < trunc),
	 * the unconditional subtraction drove cx negative, and any
	 * subsequent row->chars[cx] access read out of bounds. */
	E.buf->cx -= trunc;
	if (E.buf->cx < 0)
		E.buf->cx = 0;
}

/* Character deletion */

void delChar(int count) {
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;

	int times = UARG_COUNT(count);
	for (int i = 0; i < times; i++) {
		if (E.buf->cy == E.buf->numrows - 1 &&
		    E.buf->cx == E.buf->row[E.buf->cy].size)
			return;

		erow *row = &E.buf->row[E.buf->cy];
		if (E.buf->cx == row->size) {
			/* Deleting the row separator joins the next row
			 * onto this one. */
			const uint8_t nl = '\n';
			mutateDeleteChar(E.buf, E.buf->cx, E.buf->cy, 0,
					 E.buf->cy + 1, &nl, 1);
		} else {
			int n = utf8_nBytes(row->chars[E.buf->cx]);
			mutateDeleteChar(E.buf, E.buf->cx, E.buf->cy,
					 E.buf->cx + n, E.buf->cy,
					 &row->chars[E.buf->cx], n);
		}
	}
}

void backSpace(int count) {
	if (rejectIfReadOnly(E.buf))
		return;

	if (E.buf->mark_active && !markInvalidSilent()) {
		if (E.buf->rectangle_mode) {
			killRectangle();
			E.buf->rectangle_mode = 0;
		} else {
			deleteRange(E.buf->cx, E.buf->cy, E.buf->markx,
				    E.buf->marky, 0);
		}
		E.buf->mark_active = 0;
		return;
	}

	E.buf->mark_active = 0;
	int times = UARG_COUNT(count);
	for (int i = 0; i < times; i++) {
		if (E.buf->cy == 0 && E.buf->cx == 0)
			return;

		erow *row = &E.buf->row[E.buf->cy];
		if (E.buf->cx > 0) {
			/* Walk back over any UTF-8 continuation bytes so
			 * the whole character is one deletion. */
			int to = E.buf->cx;
			int from = to;
			do {
				from--;
			} while (from > 0 && utf8_isCont(row->chars[from]));
			mutateDeleteChar(E.buf, from, E.buf->cy, to, E.buf->cy,
					 &row->chars[from], to - from);
			E.buf->cx = from;
		} else {
			/* Joining onto the previous row deletes the
			 * separator between them. */
			const uint8_t nl = '\n';
			int prevy = E.buf->cy - 1;
			int prevx = E.buf->row[prevy].size;
			mutateDeleteChar(E.buf, prevx, prevy, 0, E.buf->cy, &nl,
					 1);
			E.buf->cx = prevx;
			E.buf->cy = prevy;
		}
	}
}

/* Word transformations */

void wordTransform(int times, uint8_t *(*transformer)(uint8_t *)) {
	times = UARG_COUNT(times);
	int icx = E.buf->cx;
	int icy = E.buf->cy;
	for (int i = 0; i < times; i++) {
		forwardWordEnd(&E.buf->cx, &E.buf->cy);
	}
	E.buf->markx = icx;
	E.buf->marky = icy;
	transformRegion(transformer);
}

/* M-- variant: transform the word before point, leaving point where
 * it is.  The case transformers are byte-length preserving, so
 * transformRange's "point at end of replacement" lands back on the
 * original position. */
static void wordTransformBackward(uint8_t *(*transformer)(uint8_t *)) {
	int icx = E.buf->cx;
	int icy = E.buf->cy;
	int sx = icx, sy = icy;
	backwardWordEnd(&sx, &sy);
	if (sx == icx && sy == icy)
		return; /* no word before point */
	transformRange(sx, sy, icx, icy, transformer);
}

static void caseWord(int uarg, uint8_t *(*transformer)(uint8_t *)) {
	if (uarg == UARG_REVERSE)
		wordTransformBackward(transformer);
	else
		wordTransform(uarg, transformer);
}

void upcaseWord(int uarg) {
	caseWord(uarg, transformerUpcase);
}

void downcaseWord(int uarg) {
	caseWord(uarg, transformerDowncase);
}

void capitalCaseWord(int uarg) {
	caseWord(uarg, transformerCapitalCase);
}

/* Word deletion */

static void deleteByWord(int count, void (*boundary)(int *, int *)) {
	if (rejectIfReadOnly(E.buf))
		return;
	E.buf->mark_active = 0;
	int startx = E.buf->cx;
	int starty = E.buf->cy;
	int times = UARG_COUNT(count);
	for (int i = 0; i < times; i++) {
		int endx = E.buf->cx;
		int endy = E.buf->cy;
		boundary(&endx, &endy);
		if (endx == E.buf->cx && endy == E.buf->cy)
			break;
		E.buf->cx = endx;
		E.buf->cy = endy;
	}
	if (E.buf->cx == startx && E.buf->cy == starty)
		return;
	int endx = E.buf->cx;
	int endy = E.buf->cy;
	E.buf->cx = startx;
	E.buf->cy = starty;
	deleteRange(E.buf->cx, E.buf->cy, endx, endy, 1);
}

void deleteWord(int count) {
	deleteByWord(count, forwardWordEnd);
}

void backspaceWord(int count) {
	deleteByWord(count, backwardWordEnd);
}

/* Character/word transposition */

/* M-- M-t: drag the word before point backward past the word before
 * it, point following the dragged word (Emacs transpose-words with a
 * negative argument). */
static void transposeWordsBackward(void) {
	/* Checked here, not just in transformRange: the point
	 * repositioning below must not run against a refused edit. */
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;
	if (bufferIsEmpty(E.buf)) {
		setStatusMessage("Buffer is empty");
		return;
	}

	int icx = E.buf->cx, icy = E.buf->cy;

	/* W2 = word ending at or before point. */
	int s2x = icx, s2y = icy;
	backwardWordEnd(&s2x, &s2y);
	if (s2x == icx && s2y == icy) {
		setStatusMessage("Cannot transpose here");
		return;
	}

	/* W1 = word before W2.  backwardWordEnd reads from point, so
	 * park point at the start of W2 for the query. */
	E.buf->cx = s2x;
	E.buf->cy = s2y;
	int s1x = s2x, s1y = s2y;
	backwardWordEnd(&s1x, &s1y);
	if (s1x == s2x && s1y == s2y) {
		E.buf->cx = icx;
		E.buf->cy = icy;
		setStatusMessage("Cannot transpose here");
		return;
	}

	/* End of W2. */
	int e2x = s2x, e2y = s2y;
	forwardWordEnd(&e2x, &e2y);

	transformRange(s1x, s1y, e2x, e2y, transformerTransposeWords);

	/* Point after the dragged word, which is now the first word of
	 * the transformed region. */
	E.buf->cx = s1x;
	E.buf->cy = s1y;
	forwardWordEnd(&E.buf->cx, &E.buf->cy);
}

void transposeWords(int uarg) {
	if (uarg == UARG_REVERSE) {
		transposeWordsBackward();
		return;
	}

	E.buf->mark_active = 0;
	if (bufferIsEmpty(E.buf)) {
		setStatusMessage("Buffer is empty");
		return;
	}

	if (E.buf->cx == 0 && E.buf->cy == 0) {
		setStatusMessage("Beginning of buffer");
		return;
	} else if (E.buf->cy == E.buf->numrows - 1 &&
		   E.buf->cx == E.buf->row[E.buf->cy].size) {
		setStatusMessage("End of buffer");
		return;
	}

	int startcx, startcy, endcx, endcy;
	backwardWordEnd(&startcx, &startcy);
	forwardWordEnd(&endcx, &endcy);
	if ((startcx == E.buf->cx && E.buf->cy == startcy) ||
	    (endcx == E.buf->cx && E.buf->cy == endcy)) {
		setStatusMessage("Cannot transpose here");
		return;
	}

	transformRange(startcx, startcy, endcx, endcy,
		       transformerTransposeWords);
}

/* M-- C-t: drag the character before point backward past the
 * character before it, point following the dragged character.  Like
 * the forward version, this stays within the current line. */
static void transposeCharsBackward(void) {
	/* Checked here, not just in transformRange: the point
	 * repositioning below must not run against a refused edit. */
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;
	if (bufferIsEmpty(E.buf)) {
		setStatusMessage("Buffer is empty");
		return;
	}

	erow *row = &E.buf->row[E.buf->cy];

	/* Need two characters before point on this line. */
	if (E.buf->cx == 0) {
		setStatusMessage("Cannot transpose here");
		return;
	}

	/* Start of the character before point... */
	int c2x = E.buf->cx - 1;
	while (c2x > 0 && utf8_isCont(row->chars[c2x]))
		c2x--;
	if (c2x == 0) {
		setStatusMessage("Cannot transpose here");
		return;
	}

	/* ...and of the character before that. */
	int c1x = c2x - 1;
	while (c1x > 0 && utf8_isCont(row->chars[c1x]))
		c1x--;

	transformRange(c1x, E.buf->cy, E.buf->cx, E.buf->cy,
		       transformerTransposeChars);

	/* Point lands after the dragged character, which is now first in
	 * the transformed range.  Re-read the row: the replace may have
	 * reallocated its chars. */
	row = &E.buf->row[E.buf->cy];
	E.buf->cx = c1x + utf8_nBytes(row->chars[c1x]);
}

void transposeChars(int uarg) {
	if (uarg == UARG_REVERSE) {
		transposeCharsBackward();
		return;
	}

	E.buf->mark_active = 0;
	if (bufferIsEmpty(E.buf)) {
		setStatusMessage("Buffer is empty");
		return;
	}

	erow *row = &E.buf->row[E.buf->cy];

	/* If nothing after point, back up one character. */
	if (E.buf->cx >= row->size) {
		if (E.buf->cx == 0) {
			/* Empty line */
			setStatusMessage("Cannot transpose here");
			return;
		}
		E.buf->cx--;
		while (E.buf->cx > 0 && utf8_isCont(row->chars[E.buf->cx]))
			E.buf->cx--;
	}

	/* Need a character before and after point. */
	if (E.buf->cx == 0 || E.buf->cx >= row->size) {
		setStatusMessage("Cannot transpose here");
		return;
	}

	/* Find the start of the character before point. */
	int startx = E.buf->cx - 1;
	while (startx > 0 && utf8_isCont(row->chars[startx]))
		startx--;

	/* Find the end of the character after point. */
	int endx = E.buf->cx + utf8_nBytes(row->chars[E.buf->cx]);

	transformRange(startx, E.buf->cy, endx, E.buf->cy,
		       transformerTransposeChars);
}

/* Line operations */

void killLine(int count) {
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;

	int times = UARG_COUNT(count);
	for (int i = 0; i < times; i++) {
		if (bufferIsEmpty(E.buf))
			return;

		erow *row = &E.buf->row[E.buf->cy];

		if (E.buf->cx == row->size) {
			/* At end of logical line: join with next line */
			delChar(1);
		} else if (E.buf->word_wrap) {
			/* Kill to end of visual sub-line */
			int display_col = charsToDisplayColumn(row, E.buf->cx);
			int current_subline, sub_col;
			cursorScreenLine(row, display_col, E.screencols,
					 &current_subline, &sub_col);
			int start_byte, end_byte;
			sublineBounds(row, E.screencols, current_subline,
				      &start_byte, &end_byte);
			if (E.buf->cx >= end_byte) {
				/* At end of sub-line: delete forward one
				 * char to pull next sub-line content up */
				delChar(1);
			} else {
				deleteRange(E.buf->cx, E.buf->cy, end_byte,
					    E.buf->cy, 1);
			}
		} else {
			/* Kill to end of logical line */
			deleteRange(E.buf->cx, E.buf->cy, row->size, E.buf->cy,
				    1);
		}
	}
}

void killLineBackwards(void) {
	E.buf->mark_active = 0;
	if (E.buf->cx == 0) {
		return;
	}

	deleteRange(0, E.buf->cy, E.buf->cx, E.buf->cy, 1);
}

void quit(void) {
	if (E.playback) {
		setStatusMessage("Not available during macro");
		return;
	}
	if (E.recording) {
		E.recording = 0;
	}
	// Check all buffers for unsaved changes, except the special buffers
	struct buffer *current = E.headbuf;
	int hasUnsavedChanges = 0;
	while (current != NULL) {
		if (current->dirty && current->filename != NULL &&
		    !current->special_buffer) {
			hasUnsavedChanges = 1;
			break;
		}
		current = current->next;
	}

	if (hasUnsavedChanges) {
		setStatusMessage(
			"There are unsaved changes. Really quit? (y or n)");
		refreshScreen();
		int c = readKey();
		if (c == 'y' || c == 'Y') {
			exit(0);
		}
		clearStatusMessage();
	} else {
		exit(0);
	}
}

/* Kill sexp (C-M-k) */

void killSexp(int count) {
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;

	int times = UARG_COUNT(count);
	for (int i = 0; i < times; i++) {
		int endx = E.buf->cx;
		int endy = E.buf->cy;
		const char *errmsg = NULL;

		if (bufferForwardSexpEnd(&endx, &endy, &errmsg) < 0) {
			setStatusMessage("%s", errmsg);
			return;
		}
		if (endx == E.buf->cx && endy == E.buf->cy)
			return;
		deleteRange(E.buf->cx, E.buf->cy, endx, endy, 1);
	}
}

/* Kill paragraph (M-k) */

void killParagraph(int count) {
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;

	int times = UARG_COUNT(count);
	for (int i = 0; i < times; i++) {
		int endx = E.buf->cx;
		int endy = E.buf->cy;
		forwardParaBoundary(&endx, &endy);
		if (endx == E.buf->cx && endy == E.buf->cy)
			return;
		deleteRange(E.buf->cx, E.buf->cy, endx, endy, 1);
	}
}

/* Mark paragraph (M-h): Emacs behavior: put point at beginning of
 * paragraph, mark at end. */

void markParagraph(void) {
	/* Find paragraph end for the mark */
	int endx = E.buf->cx;
	int endy = E.buf->cy;
	forwardParaBoundary(&endx, &endy);

	/* Find paragraph start for point */
	int startx = E.buf->cx;
	int starty = E.buf->cy;
	backwardParaBoundary(&startx, &starty);

	E.buf->markx = endx;
	E.buf->marky = endy;
	E.buf->mark_active = 1;
	E.buf->cx = startx;
	E.buf->cy = starty;

	setStatusMessage("Mark set.");
}

/* M-- C-x C-t: drag the sentence ending at or before point backward
 * past the sentence before it, point landing after the dragged
 * sentence.  Mirrors the forward version's segmentation: the gap
 * between the two sentences travels with the second segment. */
static void transposeSentencesBackward(void) {
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;

	if (bufferIsEmpty(E.buf)) {
		setStatusMessage("Buffer is empty");
		return;
	}

	/* Sentence B: ends at or before point. */
	int b_start_x = E.buf->cx, b_start_y = E.buf->cy;
	if (backwardSentenceStart(&b_start_x, &b_start_y) < 0) {
		setStatusMessage("Beginning of buffer");
		return;
	}

	/* Sentence A: the one before B. */
	int a_start_x = b_start_x, a_start_y = b_start_y;
	if (backwardSentenceStart(&a_start_x, &a_start_y) < 0) {
		setStatusMessage("Beginning of buffer");
		return;
	}

	/* A ends where the gap before B begins (same convention as the
	 * forward version: gap is folded into the B segment). */
	int a_end_x = a_start_x, a_end_y = a_start_y;
	forwardSentenceEnd(&a_end_x, &a_end_y);

	/* B end: forward from B start. */
	int b_end_x = b_start_x, b_end_y = b_start_y;
	if (forwardSentenceEnd(&b_end_x, &b_end_y) < 0) {
		setStatusMessage("End of buffer");
		return;
	}

	int a_len, b_len;
	uint8_t *a_text = collectRegionText(E.buf, a_start_x, a_start_y,
					    a_end_x, a_end_y, &a_len);
	uint8_t *b_text = collectRegionText(E.buf, a_end_x, a_end_y, b_end_x,
					    b_end_y, &b_len);

	int old_len;
	uint8_t *old_text = collectRegionText(E.buf, a_start_x, a_start_y,
					      b_end_x, b_end_y, &old_len);

	/* Build replacement: B + A */
	int repl_len = b_len + a_len;
	uint8_t *repl = xmalloc(repl_len + 1);
	memcpy(repl, b_text, b_len);
	memcpy(repl + b_len, a_text, a_len);
	repl[repl_len] = 0;

	int ex, ey;
	mutateReplace(E.buf, a_start_x, a_start_y, b_end_x, b_end_y, old_text,
		      old_len, repl, repl_len, 0, &ex, &ey);

	/* Point after the dragged sentence, now first in the region. */
	E.buf->cx = a_start_x;
	E.buf->cy = a_start_y;
	forwardSentenceEnd(&E.buf->cx, &E.buf->cy);

	free(a_text);
	free(b_text);
	free(old_text);
	free(repl);
}

/* Transpose sentences (C-x C-t): swap sentence before point with
 * sentence after point, leaving point after both. */

void transposeSentences(int uarg) {
	if (uarg == UARG_REVERSE) {
		transposeSentencesBackward();
		return;
	}

	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;

	if (bufferIsEmpty(E.buf)) {
		setStatusMessage("Buffer is empty");
		return;
	}

	/* Find boundaries of sentence A (before point) and
	 * sentence B (after point).
  	 *
	 * Layout: ... [A_start .. A_end] gap [B_start .. B_end] ...
	 * After:  ... [B] gap [A] ... with point after both.
	 */

	/* Sentence A: ends at or before point */
	int a_start_x = E.buf->cx, a_start_y = E.buf->cy;
	if (backwardSentenceStart(&a_start_x, &a_start_y) < 0) {
		setStatusMessage("Beginning of buffer");
		return;
	}

	/* Sentence B end: forward from point */
	int b_end_x = E.buf->cx, b_end_y = E.buf->cy;
	if (forwardSentenceEnd(&b_end_x, &b_end_y) < 0) {
		setStatusMessage("End of buffer");
		return;
	}

	/* Sentence A end / B start: forward from A start */
	int a_end_x = a_start_x, a_end_y = a_start_y;
	forwardSentenceEnd(&a_end_x, &a_end_y);

	/* Collect the three segments: A, gap, B */
	int a_len, gap_len, b_len;
	uint8_t *a_text = collectRegionText(E.buf, a_start_x, a_start_y,
					    a_end_x, a_end_y, &a_len);
	uint8_t *gap_text = collectRegionText(E.buf, a_end_x, a_end_y, a_end_x,
					      a_end_y, &gap_len);
	/* gap is empty when A_end == B_start; the gap is actually
	 * [a_end..b_start) but b_start == a_end in this code, so
	 * the gap is implicitly zero.  We just concatenate B + A. */
	(void)gap_len;
	free(gap_text);

	uint8_t *b_text = collectRegionText(E.buf, a_end_x, a_end_y, b_end_x,
					    b_end_y, &b_len);

	/* Collect old text for entire range */
	int old_len;
	uint8_t *old_text = collectRegionText(E.buf, a_start_x, a_start_y,
					      b_end_x, b_end_y, &old_len);

	/* Build replacement: B + A */
	int repl_len = b_len + a_len;
	uint8_t *repl = xmalloc(repl_len + 1);
	memcpy(repl, b_text, b_len);
	memcpy(repl + b_len, a_text, a_len);
	repl[repl_len] = 0;

	int ex, ey;
	mutateReplace(E.buf, a_start_x, a_start_y, b_end_x, b_end_y, old_text,
		      old_len, repl, repl_len, 0, &ex, &ey);

	E.buf->cx = ex;
	E.buf->cy = ey;

	free(a_text);
	free(b_text);
	free(old_text);
	free(repl);
}

/* Zap to char (M-z): kill from point up to and including the next
 * occurrence of a prompted character. */

void zapToChar(void) {
	if (rejectIfReadOnly(E.buf))
		return;

	E.buf->mark_active = 0;

	setStatusMessage("Zap to char: ");
	refreshScreen();

	int c = readKey();
	if (c == CTRL('g')) {
		setStatusMessage("Canceled.");
		return;
	}
	if (c == 033) {
		setStatusMessage("Canceled.");
		return;
	}

	/* The target is compared byte-for-byte against buffer content, so
	 * it has to be a single ASCII byte.*/
	if (c != '\t' && (c < ' ' || c > '~')) {
		setStatusMessage("Zap to char: not a character");
		return;
	}

	/* Search forward for the character */
	int sy = E.buf->cy;

	while (sy < E.buf->numrows) {
		erow *row = &E.buf->row[sy];
		int start = (sy == E.buf->cy) ? E.buf->cx : 0;
		for (int x = start; x < row->size; x++) {
			if (row->chars[x] == (uint8_t)c) {
				/* Skip past the target if it's not at
				 * the starting position */
				if (x == E.buf->cx && sy == E.buf->cy)
					continue;
				/* Kill up to and including this char */
				int endx = x + 1;
				int endy = sy;
				deleteRange(E.buf->cx, E.buf->cy, endx, endy,
					    1);
				return;
			}
		}
		sy++;
	}

	setStatusMessage("'%c' not found", c);
}

#include "prompt.h"
#include "unicode.h"
#include "util.h"   // For setStatusMessage
#include <stdlib.h> // For strtoul, free

/* Insert Unicode codepoint hex */
void insertCharHex(void) {
	/* Open the minibuffer prompt for input */
	uint8_t *buf = editorPrompt(E.buf, "Enter Unicode codepoint (hex): U+",
				    PROMPT_PLAIN, NULL);

	/* User pressed Ctrl-G to cancel */
	if (buf == NULL)
		return;

	/* User pressed Enter without typing anything */
	if (buf[0] == '\0') {
		free(buf);
		return;
	}

	/* Parse the hex string (e.g., "1F600" or "03B1") */
	uint32_t cp = strtoul((char *)buf, NULL, 16);
	free(buf); // Free the minibuffer result immediately

	if (cp == 0) {
		setStatusMessage("Invalid hex code");
		return;
	}

	/* Encode to UTF-8 */
	uint8_t utf8[5] = {
		0
	}; // FIXED: uint8_t instead of char to satisfy utf8Encode
	int len = utf8Encode(cp, utf8);

	if (len == 0) {
		setStatusMessage("Invalid codepoint");
		return;
	}

	memcpy(E.unicode, utf8, len);
	E.nunicode = len;

	insertUnicode(1);
}
