#include "edit.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "history.h"
#include "keymap.h"
#include "message.h"
#include "motion.h"
#include "mutate.h"
#include "prompt.h"
#include "region.h"
#include "terminal.h"
#include "transform.h"
#include "undo.h"
#include "unicode.h"
#include "unused.h"
#include "util.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

extern struct config E;

/* Character insertion */

void insertChar(struct buffer *bufr, int c, int count) {
	if (bufr->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	bufr->mark_active = 0;

	if (count <= 0)
		count = 1;

	for (int i = 0; i < count; i++) {
		if (bufr->cy == bufr->numrows) {
			insertRow(bufr, bufr->numrows, "", 0);
		}
		rowInsertChar(bufr, &bufr->row[bufr->cy], bufr->cx, c);
		bufr->cx++;
	}
}

void insertUnicode(int count) {
	E.buf->mark_active = 0;
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		undoAppendUnicode(E.buf);
		if (E.buf->cy == E.buf->numrows) {
			insertRow(E.buf, E.buf->numrows, "", 0);
		}
		rowInsertUnicode(E.buf, &E.buf->row[E.buf->cy], E.buf->cx);
		E.buf->cx += E.nunicode;
	}
}

/* Line operations */

void indentTabs(void) {
	E.buf->indent = 0;
	setStatusMessage(msg_indent_tabs);
}

void indentSpaces(void) {
	uint8_t *indentS = editorPrompt(E.buf, "Set indentation to: %s",
					PROMPT_BASIC, NULL);
	if (indentS == NULL) {
		goto cancel;
	}
	int indent = atoi((char *)indentS);
	free(indentS);
	if (indent <= 0) {
cancel:
		setStatusMessage(msg_canceled);
		return;
	}
	E.buf->indent = indent;
	setStatusMessage(msg_indent_spaces, indent);
}

void splitLineAtPoint(void) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	if (E.buf->cx == 0) {
		insertRow(E.buf, E.buf->cy, "", 0);
	} else {
		erow *row = &E.buf->row[E.buf->cy];
		insertRow(E.buf, E.buf->cy + 1, &row->chars[E.buf->cx],
			  row->size - E.buf->cx);
		row = &E.buf->row[E.buf->cy];
		row->size = E.buf->cx;
		row->chars[row->size] = '\0';
		row->cached_width = -1;
		invalidateScreenCache(E.buf);
	}
	E.buf->cy++;
	E.buf->cx = 0;
}

void insertNewline(int count) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;

	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		undoAppendChar(E.buf, '\n');
		splitLineAtPoint();
	}
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
	if (count <= 0)
		count = 1;

	for (int j = 0; j < count; j++) {
		insertNewline(1);
		erow *prev = &E.buf->row[E.buf->cy - 1];
		int i = 0;
		while (i < prev->size &&
		       (prev->chars[i] == ' ' || prev->chars[i] == CTRL('i'))) {
			undoAppendChar(E.buf, prev->chars[i]);
			insertChar(E.buf, prev->chars[i], 1);
			i++;
		}
	}
}

/* Indentation */

void editorIndent(int rept) {
	int ocx = E.buf->cx;
	int indWidth = 1;
	if (E.buf->indent) {
		indWidth = E.buf->indent;
	}
	E.buf->cx = 0;
	for (int i = 0; i < rept; i++) {
		if (E.buf->indent) {
			for (int j = 0; j < E.buf->indent; j++) {
				undoAppendChar(E.buf, ' ');
				insertChar(E.buf, ' ', 1);
			}
		} else {
			undoAppendChar(E.buf, '\t');
			insertChar(E.buf, '\t', 1);
		}
	}
	E.buf->cx = ocx + indWidth * rept;
}

void unindent(int rept) {
	if (E.buf->cy >= E.buf->numrows) {
		setStatusMessage(msg_end_of_buffer);
		return;
	}

	E.buf->mark_active = 0;

	/* Setup for indent mode */
	int indWidth = 1;
	char indCh = '\t';
	struct erow *row = &E.buf->row[E.buf->cy];
	if (E.buf->indent) {
		indWidth = E.buf->indent;
		indCh = ' ';
	}

	/* Calculate size of unindent */
	/* NB: trunc is bounded by the NUL terminator at chars[size],
	 * which always mismatches indCh (' ' or '\t'). */
	int trunc = 0;
	for (int i = 0; i < rept; i++) {
		for (int j = 0; j < indWidth; j++) {
			if (row->chars[trunc] != indCh)
				goto UNINDENT_PERFORM;
			trunc++;
		}
	}

UNINDENT_PERFORM:
	if (trunc == 0)
		return;

	/* Build old_text for mutateDelete */
	uint8_t *old_text = xmalloc(trunc + 1);
	memset(old_text, indCh, trunc);
	old_text[trunc] = 0;

	mutateDelete(E.buf, 0, E.buf->cy, trunc, E.buf->cy, old_text, trunc);
	free(old_text);

	E.buf->cx -= trunc;
}

/* Character deletion */

void delChar(int count) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;

	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		if (E.buf->cy == E.buf->numrows)
			return;
		if (E.buf->cy == E.buf->numrows - 1 &&
		    E.buf->cx == E.buf->row[E.buf->cy].size)
			return;

		erow *row = &E.buf->row[E.buf->cy];
		undoDelChar(E.buf, row);
		if (E.buf->cx == row->size) {
			row = &E.buf->row[E.buf->cy + 1];
			rowAppendString(E.buf, &E.buf->row[E.buf->cy],
					row->chars, row->size);
			delRow(E.buf, E.buf->cy + 1);
		} else {
			rowDelChar(E.buf, row, E.buf->cx);
		}
	}
}

void backSpace(int count) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		if (!E.buf->numrows)
			return;
		if (E.buf->cy == E.buf->numrows) {
			E.buf->cx = E.buf->row[--E.buf->cy].size;
			return;
		}
		if (E.buf->cy == 0 && E.buf->cx == 0)
			return;

		erow *row = &E.buf->row[E.buf->cy];
		if (E.buf->cx > 0) {
			do {
				E.buf->cx--;
				undoBackSpace(E.buf, row->chars[E.buf->cx]);
			} while (utf8_isCont(row->chars[E.buf->cx]));
			rowDelChar(E.buf, row, E.buf->cx);
		} else {
			undoBackSpace(E.buf, '\n');
			E.buf->cx = E.buf->row[E.buf->cy - 1].size;
			rowAppendString(E.buf, &E.buf->row[E.buf->cy - 1],
					row->chars, row->size);
			delRow(E.buf, E.buf->cy);
			E.buf->cy--;
		}
	}
}

/* Word transformations */

void wordTransform(int times, uint8_t *(*transformer)(uint8_t *)) {
	if (times < 1)
		times = 1;
	int icx = E.buf->cx;
	int icy = E.buf->cy;
	for (int i = 0; i < times; i++) {
		forwardWordEnd(&E.buf->cx, &E.buf->cy);
	}
	E.buf->markx = icx;
	E.buf->marky = icy;
	transformRegion(transformer);
}

void upcaseWord(int times) {
	wordTransform(times, transformerUpcase);
}

void downcaseWord(int times) {
	wordTransform(times, transformerDowncase);
}

void capitalCaseWord(int times) {
	wordTransform(times, transformerCapitalCase);
}

/* Word deletion */

static void deleteByWord(int count, void (*boundary)(int *, int *)) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}
	E.buf->mark_active = 0;
	int startx = E.buf->cx;
	int starty = E.buf->cy;
	int times = count ? count : 1;
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

void transposeWords(void) {
	E.buf->mark_active = 0;
	if (E.buf->numrows == 0) {
		setStatusMessage(msg_buffer_empty);
		return;
	}

	if (E.buf->cx == 0 && E.buf->cy == 0) {
		setStatusMessage(msg_beginning_of_buffer);
		return;
	} else if (E.buf->cy >= E.buf->numrows ||
		   (E.buf->cy == E.buf->numrows - 1 &&
		    E.buf->cx == E.buf->row[E.buf->cy].size)) {
		setStatusMessage(msg_end_of_buffer);
		return;
	}

	int startcx, startcy, endcx, endcy;
	backwardWordEnd(&startcx, &startcy);
	forwardWordEnd(&endcx, &endcy);
	if ((startcx == E.buf->cx && E.buf->cy == startcy) ||
	    (endcx == E.buf->cx && E.buf->cy == endcy)) {
		setStatusMessage(msg_cannot_transpose);
		return;
	}

	transformRange(startcx, startcy, endcx, endcy,
		       transformerTransposeWords);
}

void transposeChars(void) {
	E.buf->mark_active = 0;
	if (E.buf->numrows == 0) {
		setStatusMessage(msg_buffer_empty);
		return;
	}

	erow *row = &E.buf->row[E.buf->cy];

	/* If nothing after point, back up one character. */
	if (E.buf->cx >= row->size) {
		if (E.buf->cx == 0) {
			/* Empty line */
			setStatusMessage(msg_cannot_transpose);
			return;
		}
		E.buf->cx--;
		while (E.buf->cx > 0 && utf8_isCont(row->chars[E.buf->cx]))
			E.buf->cx--;
	}

	/* Need a character before and after point. */
	if (E.buf->cx == 0 || E.buf->cx >= row->size) {
		setStatusMessage(msg_cannot_transpose);
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
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;

	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		if (E.buf->numrows <= 0) {
			return;
		}

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
		setStatusMessage(msg_unsaved_quit);
		refreshScreen();
		int c = readKey();
		if (c == 'y' || c == 'Y') {
			exit(0);
		}
		setStatusMessage("");
	} else {
		exit(0);
	}
}

/* Kill sexp (C-M-k) */

void killSexp(int count) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;

	int times = count ? count : 1;
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
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;

	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		int endx = E.buf->cx;
		int endy = E.buf->cy;
		forwardParaBoundary(&endx, &endy);
		if (endx == E.buf->cx && endy == E.buf->cy)
			return;
		deleteRange(E.buf->cx, E.buf->cy, endx, endy, 1);
	}
}

/* Mark paragraph (M-h) — Emacs behavior: put point at beginning of
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

	setStatusMessage(msg_mark_set);
}

/* Transpose sentences (C-x C-t) — swap sentence before point with
 * sentence after point, leaving point after both. */

void transposeSentences(void) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;

	if (E.buf->numrows == 0) {
		setStatusMessage(msg_buffer_empty);
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
		setStatusMessage(msg_beginning_of_buffer);
		return;
	}

	/* Sentence B end: forward from point */
	int b_end_x = E.buf->cx, b_end_y = E.buf->cy;
	if (forwardSentenceEnd(&b_end_x, &b_end_y) < 0) {
		setStatusMessage(msg_end_of_buffer);
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
		      old_len, repl, repl_len, &ex, &ey);

	E.buf->cx = ex;
	E.buf->cy = ey;

	free(a_text);
	free(b_text);
	free(old_text);
	free(repl);
}

/* Zap to char (M-z) — kill from point up to and including the next
 * occurrence of a prompted character. */

void zapToChar(void) {
	if (E.buf->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;

	setStatusMessage("Zap to char: ");
	refreshScreen();

	int c = readKey();
	if (c == CTRL('g')) {
		setStatusMessage(msg_canceled);
		return;
	}
	if (c == 033) {
		setStatusMessage(msg_canceled);
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
