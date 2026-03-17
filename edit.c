#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "emil.h"
#include "message.h"
#include "edit.h"
#include "buffer.h"
#include "undo.h"
#include "unicode.h"
#include "display.h"
#include "keymap.h"
#include "unused.h"
#include "transform.h"
#include "region.h"
#include "prompt.h"
#include "terminal.h"
#include "history.h"
#include "util.h"
#include "adjust.h"

extern struct editorConfig E;

/* Character insertion */

void editorInsertChar(struct editorBuffer *bufr, int c, int count) {
	if (bufr->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	bufr->mark_active = 0;

	if (count <= 0)
		count = 1;

	for (int i = 0; i < count; i++) {
		if (bufr->cy == bufr->numrows) {
			editorInsertRow(bufr, bufr->numrows, "", 0);
		}
		rowInsertChar(bufr, &bufr->row[bufr->cy], bufr->cx, c);
		bufr->cx++;
	}
}

void editorInsertUnicode(struct editorBuffer *bufr, int count) {
	bufr->mark_active = 0;
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		editorUndoAppendUnicode(&E, bufr);
		if (bufr->cy == bufr->numrows) {
			editorInsertRow(bufr, bufr->numrows, "", 0);
		}
		editorRowInsertUnicode(&E, bufr, &bufr->row[bufr->cy],
				       bufr->cx);
		bufr->cx += E.nunicode;
	}
}

/* Line operations */

void editorIndentTabs(struct editorConfig *UNUSED(ed),
		      struct editorBuffer *buf) {
	buf->indent = 0;
	editorSetStatusMessage(msg_indent_tabs);
}

void editorIndentSpaces(struct editorConfig *UNUSED(ed),
			struct editorBuffer *buf) {
	uint8_t *indentS =
		editorPrompt(buf, "Set indentation to: %s", PROMPT_BASIC, NULL);
	if (indentS == NULL) {
		goto cancel;
	}
	int indent = atoi((char *)indentS);
	free(indentS);
	if (indent <= 0) {
cancel:
		editorSetStatusMessage(msg_canceled);
		return;
	}
	buf->indent = indent;
	editorSetStatusMessage(msg_indent_spaces, indent);
}

void editorInsertNewlineRaw(struct editorBuffer *bufr) {
	if (bufr->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	if (bufr->cx == 0) {
		editorInsertRow(bufr, bufr->cy, "", 0);
	} else {
		erow *row = &bufr->row[bufr->cy];
		editorInsertRow(bufr, bufr->cy + 1, &row->chars[bufr->cx],
				row->size - bufr->cx);
		row = &bufr->row[bufr->cy];
		row->size = bufr->cx;
		row->chars[row->size] = '\0';
		row->cached_width = -1;
		invalidateScreenCache(bufr);
	}
	bufr->cy++;
	bufr->cx = 0;
}

void editorInsertNewline(struct editorBuffer *bufr, int count) {
	if (bufr->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	bufr->mark_active = 0;

	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		editorUndoAppendChar(bufr, '\n');
		editorInsertNewlineRaw(bufr);
	}
}

void editorOpenLine(struct editorBuffer *bufr, int count) {
	if (count <= 0)
		count = 1;

	for (int i = 0; i < count; i++) {
		int ccx = bufr->cx;
		int ccy = bufr->cy;
		editorInsertNewline(bufr, 1);
		bufr->cx = ccx;
		bufr->cy = ccy;
	}
}

void editorInsertNewlineAndIndent(struct editorBuffer *bufr, int count) {
	if (count <= 0)
		count = 1;

	for (int j = 0; j < count; j++) {
		editorInsertNewline(bufr, 1);
		int i = 0;
		uint8_t c = bufr->row[bufr->cy - 1].chars[i];
		while (c == ' ' || c == CTRL('i')) {
			editorUndoAppendChar(bufr, c);
			editorInsertChar(bufr, c, 1);
			c = bufr->row[bufr->cy - 1].chars[++i];
		}
	}
}

/* Indentation */

void editorIndent(struct editorBuffer *bufr, int rept) {
	int ocx = bufr->cx;
	int indWidth = 1;
	if (bufr->indent) {
		indWidth = bufr->indent;
	}
	bufr->cx = 0;
	for (int i = 0; i < rept; i++) {
		if (bufr->indent) {
			for (int j = 0; j < bufr->indent; j++) {
				editorUndoAppendChar(bufr, ' ');
				editorInsertChar(bufr, ' ', 1);
			}
		} else {
			editorUndoAppendChar(bufr, '\t');
			editorInsertChar(bufr, '\t', 1);
		}
	}
	bufr->cx = ocx + indWidth * rept;
}

void editorUnindent(struct editorBuffer *bufr, int rept) {
	if (bufr->cy >= bufr->numrows) {
		editorSetStatusMessage(msg_end_of_buffer);
		return;
	}

	bufr->mark_active = 0;

	/* Setup for indent mode */
	int indWidth = 1;
	char indCh = '\t';
	struct erow *row = &bufr->row[bufr->cy];
	if (bufr->indent) {
		indWidth = bufr->indent;
		indCh = ' ';
	}

	/* Calculate size of unindent */
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

	/* Create undo */
	struct editorUndo *new = newUndo();
	new->startx = 0;
	new->starty = bufr->cy;
	new->endx = trunc;
	new->endy = bufr->cy;
	new->delete = 1;
	new->append = 0;
	pushUndo(bufr, new);
	if (new->datasize < trunc - 1) {
		new->datasize = trunc + 1;
		new->data = xrealloc(new->data, new->datasize);
	}
	memset(new->data, indCh, trunc);
	new->data[trunc] = 0;
	new->datalen = trunc;

	/* Perform row operation & dirty buffer */
	memmove(&row->chars[0], &row->chars[trunc], row->size - trunc);
	row->size -= trunc;
	row->chars[row->size] = '\0';
	bufr->cx -= trunc;
	row->cached_width = -1;
	invalidateScreenCache(bufr);
	bufr->dirty = 1;

	/* Adjust tracked points for this deletion */
	adjustAllPoints(bufr, 0, bufr->cy, trunc, bufr->cy, 1);
}

/* Character deletion */

void editorDelChar(struct editorBuffer *bufr, int count) {
	if (bufr->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	bufr->mark_active = 0;

	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		if (bufr->cy == bufr->numrows)
			return;
		if (bufr->cy == bufr->numrows - 1 &&
		    bufr->cx == bufr->row[bufr->cy].size)
			return;

		erow *row = &bufr->row[bufr->cy];
		editorUndoDelChar(bufr, row);
		if (bufr->cx == row->size) {
			row = &bufr->row[bufr->cy + 1];
			rowAppendString(bufr, &bufr->row[bufr->cy], row->chars,
					row->size);
			editorDelRow(bufr, bufr->cy + 1);
		} else {
			rowDelChar(bufr, row, bufr->cx);
		}
	}
}

/* Boundary detection */

int isParaBoundary(erow *row) {
	return (row->size == 0);
}

void editorBackSpace(struct editorBuffer *bufr, int count) {
	bufr->mark_active = 0;
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		if (!bufr->numrows)
			return;
		if (bufr->cy == bufr->numrows) {
			bufr->cx = bufr->row[--bufr->cy].size;
			return;
		}
		if (bufr->cy == 0 && bufr->cx == 0)
			return;

		erow *row = &bufr->row[bufr->cy];
		if (bufr->cx > 0) {
			do {
				bufr->cx--;
				editorUndoBackSpace(bufr, row->chars[bufr->cx]);
			} while (utf8_isCont(row->chars[bufr->cx]));
			rowDelChar(bufr, row, bufr->cx);
		} else {
			editorUndoBackSpace(bufr, '\n');
			bufr->cx = bufr->row[bufr->cy - 1].size;
			rowAppendString(bufr, &bufr->row[bufr->cy - 1],
					row->chars, row->size);
			editorDelRow(bufr, bufr->cy);
			bufr->cy--;
		}
	}
}

/* Cursor movement */

/* Move cursor up or down by one visual (screen) row when word wrap is
 * active.  direction: -1 = up, +1 = down. */
static void editorMoveVisualRow(int direction) {
	struct editorBuffer *buf = E.buf;
	if (buf->cy >= buf->numrows)
		return;

	erow *row = &buf->row[buf->cy];
	int display_col = charsToDisplayColumn(row, buf->cx);

	int current_subline, sub_col;
	cursorScreenLine(row, display_col, E.screencols, &current_subline,
			 &sub_col);

	int target_subline = current_subline + direction;
	int total_sublines = countScreenLines(row, E.screencols);

	if (target_subline >= 0 && target_subline < total_sublines) {
		/* Move within the same logical row */
		buf->cx = displayColumnToByteOffset(row, E.screencols,
						    target_subline, sub_col);
	} else if (target_subline < 0) {
		/* Move to previous logical row */
		if (buf->cy == 0)
			return;
		buf->cy--;
		erow *prev = &buf->row[buf->cy];
		int last_sub = countScreenLines(prev, E.screencols) - 1;
		buf->cx = displayColumnToByteOffset(prev, E.screencols,
						    last_sub, sub_col);
	} else {
		/* Move to next logical row */
		if (buf->cy >= buf->numrows - 1) {
			/* Allow moving to the virtual line past EOF */
			buf->cy = buf->numrows;
			buf->cx = 0;
			return;
		}
		buf->cy++;
		erow *next = &buf->row[buf->cy];
		buf->cx = displayColumnToByteOffset(next, E.screencols, 0,
						    sub_col);
	}
}

/* * TODO: Refactor to take struct editorBuffer *buf parameter.
 * Extract UTF-8 snap-to-character-boundary into a helper function
 * with consistent bounds checking across all four directions. */
void editorMoveCursor(int key, int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		erow *row = (E.buf->cy >= E.buf->numrows) ?
				    NULL :
				    &E.buf->row[E.buf->cy];

		switch (key) {
		case KEY_ARROW_LEFT:
			if (E.buf->cx != 0) {
				do
					E.buf->cx--;
				while (E.buf->cx != 0 &&
				       utf8_isCont(row->chars[E.buf->cx]));
			} else if (E.buf->cy > 0) {
				E.buf->cy--;
				E.buf->cx = E.buf->row[E.buf->cy].size;
			}
			break;

		case KEY_ARROW_RIGHT:
			if (row && E.buf->cx < row->size) {
				E.buf->cx += utf8_nBytes(row->chars[E.buf->cx]);
			} else if (row && E.buf->cx == row->size) {
				E.buf->cy++;
				E.buf->cx = 0;
			}
			break;
		case KEY_ARROW_UP:
			if (E.buf->word_wrap) {
				editorMoveVisualRow(-1);
			} else if (E.buf->cy > 0) {
				E.buf->cy--;
				if (E.buf->row[E.buf->cy].chars == NULL)
					break;
				while (utf8_isCont(
					E.buf->row[E.buf->cy].chars[E.buf->cx]))
					E.buf->cx++;
			}
			break;
		case KEY_ARROW_DOWN:
			if (E.buf->word_wrap) {
				editorMoveVisualRow(+1);
			} else if (E.buf->cy < E.buf->numrows) {
				E.buf->cy++;
				if (E.buf->cy < E.buf->numrows) {
					if (E.buf->row[E.buf->cy].chars == NULL)
						break;
					while (E.buf->cx < E.buf->row[E.buf->cy]
								   .size &&
					       utf8_isCont(
						       E.buf->row[E.buf->cy]
							       .chars[E.buf->cx]))
						E.buf->cx++;
				} else {
					E.buf->cx = 0;
				}
			}
			break;
		}
		row = (E.buf->cy >= E.buf->numrows) ? NULL :
						      &E.buf->row[E.buf->cy];
		int rowlen = row ? row->size : 0;
		if (E.buf->cx > rowlen) {
			E.buf->cx = rowlen;
		}
	}
}

/* Word movement */

void bufferEndOfForwardWord(struct editorBuffer *buf, int *dx, int *dy) {
	int cx = buf->cx;
	int icy = buf->cy;
	if (icy >= buf->numrows) {
		*dx = cx;
		*dy = icy;
		return;
	}
	int pre = 1;
	for (int cy = icy; cy < buf->numrows; cy++) {
		int l = buf->row[cy].size;
		while (cx < l) {
			uint8_t c = buf->row[cy].chars[cx];
			if (isWordBoundary(c) && !pre) {
				*dx = cx;
				*dy = cy;
				return;
			} else if (!isWordBoundary(c)) {
				pre = 0;
			}
			cx++;
		}
		if (!pre) {
			*dx = cx;
			*dy = cy;
			return;
		}
		cx = 0;
	}
	*dx = cx;
	*dy = icy;
}

void bufferEndOfBackwardWord(struct editorBuffer *buf, int *dx, int *dy) {
	int cx = buf->cx;
	int icy = buf->cy;

	if (icy >= buf->numrows) {
		return;
	}

	int pre = 1;

	for (int cy = icy; cy >= 0; cy--) {
		if (cy != icy) {
			cx = buf->row[cy].size;
		}
		while (cx > 0) {
			uint8_t c = buf->row[cy].chars[cx - 1];
			if (isWordBoundary(c) && !pre) {
				*dx = cx;
				*dy = cy;
				return;
			} else if (!isWordBoundary(c)) {
				pre = 0;
			}
			cx--;
		}
		if (!pre) {
			*dx = cx;
			*dy = cy;
			return;
		}
	}

	*dx = cx;
	*dy = 0;
}

void editorForwardWord(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		bufferEndOfForwardWord(E.buf, &E.buf->cx, &E.buf->cy);
	}
}

void editorBackWord(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		bufferEndOfBackwardWord(E.buf, &E.buf->cx, &E.buf->cy);
	}
}

/* Paragraph movement */

/* Buffer-parameterized helpers for paragraph boundary scanning.
 * These set cx=0 and update *cy to the boundary line. */

void bufferBackwardParagraphBoundary(struct editorBuffer *buf, int *cx,
				     int *cy) {
	*cx = 0;
	int icy = *cy;

	if (icy >= buf->numrows) {
		icy--;
	}

	if (buf->numrows == 0) {
		return;
	}

	int pre = 1;

	for (int y = icy; y >= 0; y--) {
		erow *row = &buf->row[y];
		if (isParaBoundary(row) && !pre) {
			*cy = y;
			return;
		} else if (!isParaBoundary(row)) {
			pre = 0;
		}
	}

	*cy = 0;
}

void bufferForwardParagraphBoundary(struct editorBuffer *buf, int *cx,
				    int *cy) {
	*cx = 0;
	int icy = *cy;

	if (icy >= buf->numrows) {
		return;
	}

	if (buf->numrows == 0) {
		return;
	}

	int pre = 1;

	for (int y = icy; y < buf->numrows; y++) {
		erow *row = &buf->row[y];
		if (isParaBoundary(row) && !pre) {
			*cy = y;
			return;
		} else if (!isParaBoundary(row)) {
			pre = 0;
		}
	}

	*cy = buf->numrows;
}

void editorBackPara(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		bufferBackwardParagraphBoundary(E.buf, &E.buf->cx, &E.buf->cy);
	}
}

void editorForwardPara(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		bufferForwardParagraphBoundary(E.buf, &E.buf->cx, &E.buf->cy);
	}
}

/* Sexp (balanced expression) movement — C-M-f / C-M-b */

static int matchingClose(uint8_t c) {
	switch (c) {
	case '(':
		return ')';
	case '[':
		return ']';
	case '{':
		return '}';
	default:
		return 0;
	}
}

static int matchingOpen(uint8_t c) {
	switch (c) {
	case ')':
		return '(';
	case ']':
		return '[';
	case '}':
		return '{';
	default:
		return 0;
	}
}

static int isQuoteChar(uint8_t c) {
	return c == '"' || c == '\'';
}

/* Advance one position forward in the buffer.  Returns 0 at end. */
static int stepForward(struct editorBuffer *buf, int *cx, int *cy) {
	if (*cy >= buf->numrows)
		return 0;
	if (*cx < buf->row[*cy].size) {
		(*cx)++;
		return 1;
	}
	if (*cy + 1 < buf->numrows) {
		*cy += 1;
		*cx = 0;
		return 1;
	}
	return 0;
}

/* Retreat one position backward in the buffer.  Returns 0 at start. */
static int stepBackward(struct editorBuffer *buf, int *cx, int *cy) {
	if (*cx > 0) {
		(*cx)--;
		return 1;
	}
	if (*cy > 0) {
		*cy -= 1;
		*cx = buf->row[*cy].size;
		return 1;
	}
	return 0;
}

/* Get the character at (cx, cy).  Returns 0 at end-of-line / end-of-buffer.
 * End-of-line positions (cx == row->size) are treated as newline. */
static uint8_t charAt(struct editorBuffer *buf, int cx, int cy) {
	if (cy >= buf->numrows)
		return 0;
	erow *row = &buf->row[cy];
	if (cx >= row->size)
		return '\n';
	return row->chars[cx];
}

/* Scan forward from (*cx, *cy) past one sexp (balanced expression).
 * On success, updates (*cx, *cy) to just past the sexp and returns 0.
 * On failure (unmatched delimiter, end of buffer), returns -1 without
 * modifying *cx / *cy; *errmsg is set to a description. */
static int bufferForwardSexpEnd(struct editorBuffer *buf, int *cx, int *cy,
				const char **errmsg) {
	int px = *cx, py = *cy;

	/* Skip whitespace and newlines */
	while (py < buf->numrows) {
		uint8_t ch = charAt(buf, px, py);
		if (ch == 0) {
			*errmsg = "End of buffer";
			return -1;
		}
		if (ch != ' ' && ch != '\t' && ch != '\n')
			break;
		stepForward(buf, &px, &py);
	}
	if (py >= buf->numrows) {
		*errmsg = "End of buffer";
		return -1;
	}

	uint8_t ch = charAt(buf, px, py);

	/* Opening delimiter: scan forward for matching close */
	int close = matchingClose(ch);
	if (close) {
		int depth = 1;
		int sx = px, sy = py;
		stepForward(buf, &sx, &sy);
		while (depth > 0) {
			uint8_t c = charAt(buf, sx, sy);
			if (c == 0) {
				*errmsg = "Unmatched delimiter";
				return -1;
			}
			if ((int)c == close)
				depth--;
			else if (c == ch)
				depth++;
			if (depth > 0)
				stepForward(buf, &sx, &sy);
		}
		/* Land after the closing delimiter */
		stepForward(buf, &sx, &sy);
		*cx = sx;
		*cy = sy;
		return 0;
	}

	/* Closing delimiter while inside: jump past it */
	if (matchingOpen(ch)) {
		stepForward(buf, &px, &py);
		*cx = px;
		*cy = py;
		return 0;
	}

	/* Quote character: scan forward for matching quote */
	if (isQuoteChar(ch)) {
		int sx = px, sy = py;
		stepForward(buf, &sx, &sy);
		while (1) {
			uint8_t c = charAt(buf, sx, sy);
			if (c == 0) {
				*errmsg = "Unmatched quote";
				return -1;
			}
			if (c == ch) {
				stepForward(buf, &sx, &sy);
				*cx = sx;
				*cy = sy;
				return 0;
			}
			stepForward(buf, &sx, &sy);
		}
	}

	/* Word: skip to end of word */
	*cx = px;
	*cy = py;
	bufferEndOfForwardWord(buf, cx, cy);
	return 0;
}

void editorForwardSexp(int count) {
	int times = count ? count : 1;
	struct editorBuffer *buf = E.buf;

	for (int t = 0; t < times; t++) {
		int cx = buf->cx;
		int cy = buf->cy;
		const char *errmsg = NULL;

		if (bufferForwardSexpEnd(buf, &cx, &cy, &errmsg) < 0) {
			editorSetStatusMessage("%s", errmsg);
			return;
		}
		buf->cx = cx;
		buf->cy = cy;
	}
}

void editorBackwardSexp(int count) {
	int times = count ? count : 1;
	struct editorBuffer *buf = E.buf;

	for (int t = 0; t < times; t++) {
		int cx = buf->cx;
		int cy = buf->cy;

		/* Step back once then skip whitespace and newlines */
		if (!stepBackward(buf, &cx, &cy)) {
			editorSetStatusMessage(msg_beginning_of_buffer);
			return;
		}
		while (1) {
			uint8_t ch = charAt(buf, cx, cy);
			if (ch != ' ' && ch != '\t' && ch != '\n')
				break;
			if (!stepBackward(buf, &cx, &cy)) {
				editorSetStatusMessage(msg_beginning_of_buffer);
				return;
			}
		}

		uint8_t ch = charAt(buf, cx, cy);

		/* Closing delimiter: scan backward for matching open */
		int open = matchingOpen(ch);
		if (open) {
			int depth = 1;
			int sx = cx, sy = cy;
			while (depth > 0) {
				if (!stepBackward(buf, &sx, &sy)) {
					editorSetStatusMessage(
						"Unmatched delimiter");
					return;
				}
				uint8_t c = charAt(buf, sx, sy);
				if ((int)c == open)
					depth--;
				else if (c == ch)
					depth++;
			}
			buf->cx = sx;
			buf->cy = sy;
			continue;
		}

		/* Opening delimiter while inside: land on it */
		if (matchingClose(ch)) {
			buf->cx = cx;
			buf->cy = cy;
			continue;
		}

		/* Quote character: scan backward for matching quote */
		if (isQuoteChar(ch)) {
			int sx = cx, sy = cy;
			while (1) {
				if (!stepBackward(buf, &sx, &sy)) {
					editorSetStatusMessage(
						"Unmatched quote");
					return;
				}
				uint8_t c = charAt(buf, sx, sy);
				if (c == ch) {
					buf->cx = sx;
					buf->cy = sy;
					break;
				}
			}
			continue;
		}

		/* Word: skip to beginning of word */
		buf->cx = cx;
		buf->cy = cy;
		/* stepForward to undo the stepBackward, then use word movement */
		stepForward(buf, &buf->cx, &buf->cy);
		bufferEndOfBackwardWord(buf, &buf->cx, &buf->cy);
	}
}

/* Word transformations */

void wordTransform(struct editorBuffer *bufr, int times,
		   uint8_t *(*transformer)(uint8_t *)) {
	int icx = bufr->cx;
	int icy = bufr->cy;
	for (int i = 0; i < times; i++) {
		bufferEndOfForwardWord(bufr, &bufr->cx, &bufr->cy);
	}
	bufr->markx = icx;
	bufr->marky = icy;
	editorTransformRegion(&E, bufr, transformer);
}

void editorUpcaseWord(struct editorBuffer *bufr, int times) {
	wordTransform(bufr, times, transformerUpcase);
}

void editorDowncaseWord(struct editorBuffer *bufr, int times) {
	wordTransform(bufr, times, transformerDowncase);
}

void editorCapitalCaseWord(struct editorBuffer *bufr, int times) {
	wordTransform(bufr, times, transformerCapitalCase);
}

/* Word deletion */

void editorDeleteWord(struct editorBuffer *bufr, int count) {
	if (bufr->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}
	bufr->mark_active = 0;
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		int endx = bufr->cx;
		int endy = bufr->cy;
		bufferEndOfForwardWord(bufr, &endx, &endy);
		if (endx == bufr->cx && endy == bufr->cy)
			return;
		editorDeleteRange(bufr, bufr->cx, bufr->cy, endx, endy, 1);
	}
}

void editorBackspaceWord(struct editorBuffer *bufr, int count) {
	if (bufr->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}
	bufr->mark_active = 0;
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		int endx = bufr->cx;
		int endy = bufr->cy;
		bufferEndOfBackwardWord(bufr, &endx, &endy);
		if (endx == bufr->cx && endy == bufr->cy)
			return;
		editorDeleteRange(bufr, bufr->cx, bufr->cy, endx, endy, 1);
	}
}

/* Character/word transposition */

void editorTransposeWords(struct editorBuffer *bufr) {
	bufr->mark_active = 0;
	if (bufr->numrows == 0) {
		editorSetStatusMessage(msg_buffer_empty);
		return;
	}

	if (bufr->cx == 0 && bufr->cy == 0) {
		editorSetStatusMessage(msg_beginning_of_buffer);
		return;
	} else if (bufr->cy >= bufr->numrows ||
		   (bufr->cy == bufr->numrows - 1 &&
		    bufr->cx == bufr->row[bufr->cy].size)) {
		editorSetStatusMessage(msg_end_of_buffer);
		return;
	}

	int startcx, startcy, endcx, endcy;
	bufferEndOfBackwardWord(bufr, &startcx, &startcy);
	bufferEndOfForwardWord(bufr, &endcx, &endcy);
	if ((startcx == bufr->cx && bufr->cy == startcy) ||
	    (endcx == bufr->cx && bufr->cy == endcy)) {
		editorSetStatusMessage(msg_cannot_transpose);
		return;
	}

	editorTransformRange(&E, bufr, startcx, startcy, endcx, endcy,
			     transformerTransposeWords);
}

void editorTransposeChars(struct editorBuffer *bufr) {
	bufr->mark_active = 0;
	if (bufr->numrows == 0) {
		editorSetStatusMessage(msg_buffer_empty);
		return;
	}

	erow *row = &bufr->row[bufr->cy];

	/* If nothing after point, back up one character. */
	if (bufr->cx >= row->size) {
		if (bufr->cx == 0) {
			/* Empty line */
			editorSetStatusMessage(msg_cannot_transpose);
			return;
		}
		bufr->cx--;
		while (bufr->cx > 0 && utf8_isCont(row->chars[bufr->cx]))
			bufr->cx--;
	}

	/* Need a character before and after point. */
	if (bufr->cx == 0 || bufr->cx >= row->size) {
		editorSetStatusMessage(msg_cannot_transpose);
		return;
	}

	/* Find the start of the character before point. */
	int startx = bufr->cx - 1;
	while (startx > 0 && utf8_isCont(row->chars[startx]))
		startx--;

	/* Find the end of the character after point. */
	int endx = bufr->cx + utf8_nBytes(row->chars[bufr->cx]);

	editorTransformRange(&E, bufr, startx, bufr->cy, endx, bufr->cy,
			     transformerTransposeChars);
}

/* Line operations */

void editorKillLine(int count) {
	if (E.buf->read_only) {
		editorSetStatusMessage(msg_read_only);
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
			editorDelChar(E.buf, 1);
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
				editorDelChar(E.buf, 1);
			} else {
				editorDeleteRange(E.buf, E.buf->cx, E.buf->cy,
						  end_byte, E.buf->cy, 1);
			}
		} else {
			/* Kill to end of logical line */
			editorDeleteRange(E.buf, E.buf->cx, E.buf->cy,
					  row->size, E.buf->cy, 1);
		}
	}
}

void editorKillLineBackwards(void) {
	E.buf->mark_active = 0;
	if (E.buf->cx == 0) {
		return;
	}

	editorDeleteRange(E.buf, 0, E.buf->cy, E.buf->cx, E.buf->cy, 1);
}

/* Navigation */

void editorPageUp(int count) {
	struct editorWindow *win = E.windows[windowFocusedIdx()];
	int times = count ? count : 1;

	for (int n = 0; n < times; n++) {
		int scroll_lines = win->height - page_overlap;
		if (scroll_lines < 1)
			scroll_lines = 1;

		scrollViewport(win, E.buf, -scroll_lines);
		clampCursorToViewport(win, E.buf);
	}
}

void editorPageDown(int count) {
	struct editorWindow *win = E.windows[windowFocusedIdx()];
	int times = count ? count : 1;

	for (int n = 0; n < times; n++) {
		int scroll_lines = win->height - page_overlap;
		if (scroll_lines < 1)
			scroll_lines = 1;

		scrollViewport(win, E.buf, scroll_lines);
		clampCursorToViewport(win, E.buf);
	}
}

void editorScrollLineUp(int count) {
	struct editorWindow *win = E.windows[windowFocusedIdx()];
	int times = count ? count : 1;

	scrollViewport(win, E.buf, -times);
	clampCursorToViewport(win, E.buf);
}

void editorScrollLineDown(int count) {
	struct editorWindow *win = E.windows[windowFocusedIdx()];
	int times = count ? count : 1;

	scrollViewport(win, E.buf, times);
	clampCursorToViewport(win, E.buf);
}

void editorBeginningOfLine(int count) {
	if (count != 0) {
		editorKillLineBackwards();
		return;
	}

	if (E.buf->word_wrap && E.buf->cy < E.buf->numrows) {
		erow *row = &E.buf->row[E.buf->cy];
		int display_col = charsToDisplayColumn(row, E.buf->cx);
		int current_subline, sub_col;
		cursorScreenLine(row, display_col, E.screencols,
				 &current_subline, &sub_col);
		int start_byte, end_byte;
		sublineBounds(row, E.screencols, current_subline, &start_byte,
			      &end_byte);
		E.buf->cx = start_byte;
	} else {
		E.buf->cx = 0;
	}
}

void editorEndOfLine(int count) {
	(void)count;
	if (E.buf->row == NULL || E.buf->cy >= E.buf->numrows)
		return;

	erow *row = &E.buf->row[E.buf->cy];

	if (E.buf->word_wrap) {
		int display_col = charsToDisplayColumn(row, E.buf->cx);
		int current_subline, sub_col;
		cursorScreenLine(row, display_col, E.screencols,
				 &current_subline, &sub_col);
		int start_byte, end_byte;
		sublineBounds(row, E.screencols, current_subline, &start_byte,
			      &end_byte);
		if (end_byte < row->size) {
			/* Mid-row sub-line: back up one character so we
			 * land on the last char of this sub-line, not
			 * the first char of the next one. */
			int pos = end_byte;
			do
				pos--;
			while (pos > start_byte &&
			       utf8_isCont(row->chars[pos]));
			E.buf->cx = pos;
		} else {
			E.buf->cx = row->size;
		}
	} else {
		E.buf->cx = row->size;
	}
}

void editorQuit(void) {
	if (E.recording) {
		E.recording = 0;
	}
	// Check all buffers for unsaved changes, except the special buffers
	struct editorBuffer *current = E.headbuf;
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
		editorSetStatusMessage(msg_unsaved_quit);
		refreshScreen();
		int c = editorReadKey();
		if (c == 'y' || c == 'Y') {
			exit(0);
		}
		editorSetStatusMessage("");
	} else {
		exit(0);
	}
}

void editorGotoLine(void) {
	uint8_t *nls;
	int nl;

	for (;;) {
		nls = editorPrompt(E.buf, "Goto line: %s", PROMPT_BASIC, NULL);
		if (!nls) {
			return;
		}

		nl = atoi((char *)nls);
		free(nls);

		if (nl) {
			E.buf->cx = 0;
			if (nl < 0) {
				E.buf->cy = 0;
			} else if (nl > E.buf->numrows) {
				E.buf->cy = E.buf->numrows;
			} else {
				E.buf->cy = nl - 1;
			}
			return;
		}
	}
}

/* Sentence movement */

static int isSentenceEnd(uint8_t c) {
	return c == '.' || c == '!' || c == '?';
}

static int isClosingPunct(uint8_t c) {
	return c == ')' || c == ']' || c == '"' || c == '\'';
}

/* Scan forward from (*cx, *cy) to the position just after the end of
 * the current sentence.  A sentence ends at . ! ? optionally followed
 * by closing punctuation, then followed by whitespace, newline, or
 * end-of-buffer.  A paragraph boundary (empty line) also ends a sentence.
 * Returns 0 on success (position updated), -1 at end-of-buffer. */
int bufferForwardSentenceEnd(struct editorBuffer *buf, int *cx, int *cy) {
	int px = *cx, py = *cy;

	while (py < buf->numrows) {
		erow *row = &buf->row[py];

		while (px < row->size) {
			uint8_t c = row->chars[px];
			if (isSentenceEnd(c)) {
				/* Skip past optional closing punctuation */
				int sx = px + 1;
				while (sx < row->size &&
				       isClosingPunct(row->chars[sx]))
					sx++;
				/* Sentence ends if followed by whitespace,
				 * end-of-line, or end-of-buffer */
				if (sx >= row->size || row->chars[sx] == ' ' ||
				    row->chars[sx] == '\t') {
					/* Land after the whitespace */
					if (sx < row->size) {
						/* Skip one whitespace char */
						sx++;
					} else if (py + 1 < buf->numrows) {
						/* End of line — move to start
						 * of next line */
						py++;
						sx = 0;
					} else {
						/* End of buffer */
					}
					*cx = sx;
					*cy = py;
					return 0;
				}
			}
			px++;
		}

		/* Check for paragraph boundary (next line empty) */
		if (py + 1 < buf->numrows &&
		    isParaBoundary(&buf->row[py + 1])) {
			/* Sentence ends at end of this line */
			*cx = row->size;
			*cy = py;
			/* Advance past the blank line(s) to match Emacs
			 * behavior: land on first non-blank line */
			py++;
			while (py < buf->numrows &&
			       isParaBoundary(&buf->row[py]))
				py++;
			*cx = 0;
			*cy = py < buf->numrows ? py : buf->numrows;
			return 0;
		}

		py++;
		px = 0;
	}

	/* Reached end of buffer */
	*cx = 0;
	*cy = buf->numrows;
	return -1;
}

/* Scan backward from (*cx, *cy) to the beginning of the current sentence.
 * Returns 0 on success, -1 at beginning-of-buffer. */
int bufferBackwardSentenceStart(struct editorBuffer *buf, int *cx, int *cy) {
	int px = *cx, py = *cy;

	if (py >= buf->numrows) {
		if (buf->numrows == 0)
			return -1;
		py = buf->numrows - 1;
		px = buf->row[py].size;
	}

	/* Step back once to avoid detecting the current position */
	if (!stepBackward(buf, &px, &py))
		return -1;

	/* Skip whitespace/newlines backward */
	while (1) {
		uint8_t c = charAt(buf, px, py);
		if (c != ' ' && c != '\t' && c != '\n')
			break;
		if (!stepBackward(buf, &px, &py)) {
			*cx = 0;
			*cy = 0;
			return 0;
		}
	}

	/* Skip closing punctuation and sentence terminators backward
	 * so we don't re-detect the end of the sentence we're at. */
	while (1) {
		uint8_t c = charAt(buf, px, py);
		if (!isSentenceEnd(c) && !isClosingPunct(c))
			break;
		if (!stepBackward(buf, &px, &py)) {
			*cx = 0;
			*cy = 0;
			return 0;
		}
	}

	/* Now scan backward to find the previous sentence end or
	 * paragraph boundary, then position after it. */
	while (1) {
		/* Check for paragraph boundary — blank line means
		 * sentence starts on the line after the blank */
		if (py < buf->numrows && isParaBoundary(&buf->row[py])) {
			/* Land on the first non-blank line after */
			int ny = py;
			while (ny < buf->numrows &&
			       isParaBoundary(&buf->row[ny]))
				ny++;
			*cx = 0;
			*cy = ny < buf->numrows ? ny : py;
			return 0;
		}

		uint8_t c = charAt(buf, px, py);
		if (isSentenceEnd(c)) {
			/* Found a sentence terminator — sentence starts
			 * after this terminator plus any closing punct
			 * and one whitespace char */
			int sx = px, sy = py;
			stepForward(buf, &sx, &sy);
			/* Skip closing punctuation */
			while (sy < buf->numrows) {
				uint8_t nc = charAt(buf, sx, sy);
				if (!isClosingPunct(nc))
					break;
				stepForward(buf, &sx, &sy);
			}
			/* Skip one whitespace */
			if (sy < buf->numrows) {
				uint8_t nc = charAt(buf, sx, sy);
				if (nc == ' ' || nc == '\t' || nc == '\n')
					stepForward(buf, &sx, &sy);
			}
			*cx = sx;
			*cy = sy;
			return 0;
		}

		if (!stepBackward(buf, &px, &py)) {
			*cx = 0;
			*cy = 0;
			return 0;
		}
	}
}

void editorForwardSentence(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		bufferForwardSentenceEnd(E.buf, &E.buf->cx, &E.buf->cy);
	}
}

void editorBackwardSentence(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		bufferBackwardSentenceStart(E.buf, &E.buf->cx, &E.buf->cy);
	}
}

/* Kill sexp (C-M-k) */

void editorKillSexp(int count) {
	if (E.buf->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;

	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		int endx = E.buf->cx;
		int endy = E.buf->cy;
		const char *errmsg = NULL;

		if (bufferForwardSexpEnd(E.buf, &endx, &endy, &errmsg) < 0) {
			editorSetStatusMessage("%s", errmsg);
			return;
		}
		if (endx == E.buf->cx && endy == E.buf->cy)
			return;
		editorDeleteRange(E.buf, E.buf->cx, E.buf->cy, endx, endy, 1);
	}
}

/* Kill paragraph (M-k) */

void editorKillParagraph(int count) {
	if (E.buf->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	E.buf->mark_active = 0;

	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		int endx = E.buf->cx;
		int endy = E.buf->cy;
		bufferForwardParagraphBoundary(E.buf, &endx, &endy);
		if (endx == E.buf->cx && endy == E.buf->cy)
			return;
		editorDeleteRange(E.buf, E.buf->cx, E.buf->cy, endx, endy, 1);
	}
}

/* Mark paragraph (M-h) — Emacs behavior: put point at beginning of
 * paragraph, mark at end. */

void editorMarkParagraph(void) {
	struct editorBuffer *buf = E.buf;

	/* Find paragraph end for the mark */
	int endx = buf->cx;
	int endy = buf->cy;
	bufferForwardParagraphBoundary(buf, &endx, &endy);

	/* Find paragraph start for point */
	int startx = buf->cx;
	int starty = buf->cy;
	bufferBackwardParagraphBoundary(buf, &startx, &starty);

	buf->markx = endx;
	buf->marky = endy;
	buf->mark_active = 1;
	buf->cx = startx;
	buf->cy = starty;

	editorSetStatusMessage(msg_mark_set);
}

/* Transpose sentences (C-x C-t) — swap sentence before point with
 * sentence after point, leaving point after both. */

void editorTransposeSentences(struct editorBuffer *bufr) {
	if (bufr->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	bufr->mark_active = 0;

	if (bufr->numrows == 0) {
		editorSetStatusMessage(msg_buffer_empty);
		return;
	}

	/* Find boundaries of sentence A (before point) and
	 * sentence B (after point).
	 *
	 * Layout: ... [A_start .. A_end] gap [B_start .. B_end] ...
	 * After:  ... [B] gap [A] ... with point after both.
	 */

	/* Sentence A: ends at or before point */
	int a_start_x = bufr->cx, a_start_y = bufr->cy;
	if (bufferBackwardSentenceStart(bufr, &a_start_x, &a_start_y) < 0) {
		editorSetStatusMessage(msg_beginning_of_buffer);
		return;
	}

	/* Sentence B end: forward from point */
	int b_end_x = bufr->cx, b_end_y = bufr->cy;
	if (bufferForwardSentenceEnd(bufr, &b_end_x, &b_end_y) < 0) {
		editorSetStatusMessage(msg_end_of_buffer);
		return;
	}

	/* Sentence A end / B start: forward from A start */
	int a_end_x = a_start_x, a_end_y = a_start_y;
	bufferForwardSentenceEnd(bufr, &a_end_x, &a_end_y);

	/* Set region to span from A start to B end and use the
	 * transpose-words transformer on the sentence boundaries.
	 * Actually, we do a manual swap: extract both sentences,
	 * delete the range, re-insert in swapped order. */

	/* Extract sentence A text */
	int a_len = 0;
	char *a_text = NULL;
	{
		int sx = a_start_x, sy = a_start_y;
		/* Calculate length */
		int tx = sx, ty = sy;
		while (ty < a_end_y || (ty == a_end_y && tx < a_end_x)) {
			a_len++;
			stepForward(bufr, &tx, &ty);
		}
		a_text = xmalloc(a_len + 1);
		tx = sx;
		ty = sy;
		for (int i = 0; i < a_len; i++) {
			a_text[i] = charAt(bufr, tx, ty);
			stepForward(bufr, &tx, &ty);
		}
		a_text[a_len] = '\0';
	}

	/* Extract sentence B text */
	int b_len = 0;
	char *b_text = NULL;
	{
		int sx = a_end_x, sy = a_end_y;
		int tx = sx, ty = sy;
		while (ty < b_end_y || (ty == b_end_y && tx < b_end_x)) {
			b_len++;
			stepForward(bufr, &tx, &ty);
		}
		b_text = xmalloc(b_len + 1);
		tx = sx;
		ty = sy;
		for (int i = 0; i < b_len; i++) {
			b_text[i] = charAt(bufr, tx, ty);
			stepForward(bufr, &tx, &ty);
		}
		b_text[b_len] = '\0';
	}

	/* Delete the entire range from A start to B end */
	editorDeleteRange(bufr, a_start_x, a_start_y, b_end_x, b_end_y, 0);

	/* Insert B text then A text at the deletion point */
	bufr->cx = a_start_x;
	bufr->cy = a_start_y;
	for (int i = 0; i < b_len; i++) {
		if (b_text[i] == '\n') {
			editorInsertNewlineRaw(bufr);
		} else {
			editorInsertChar(bufr, (uint8_t)b_text[i], 1);
		}
	}
	for (int i = 0; i < a_len; i++) {
		if (a_text[i] == '\n') {
			editorInsertNewlineRaw(bufr);
		} else {
			editorInsertChar(bufr, (uint8_t)a_text[i], 1);
		}
	}

	free(a_text);
	free(b_text);
}

/* Zap to char (M-z) — kill from point up to and including the next
 * occurrence of a prompted character. */

void editorZapToChar(struct editorBuffer *bufr) {
	if (bufr->read_only) {
		editorSetStatusMessage(msg_read_only);
		return;
	}

	bufr->mark_active = 0;

	editorSetStatusMessage("Zap to char: ");
	refreshScreen();

	int c = editorReadKey();
	if (c == CTRL('g')) {
		editorSetStatusMessage(msg_canceled);
		return;
	}
	if (c == 033) {
		editorSetStatusMessage(msg_canceled);
		return;
	}

	/* Search forward for the character */
	int sy = bufr->cy;

	while (sy < bufr->numrows) {
		erow *row = &bufr->row[sy];
		int start = (sy == bufr->cy) ? bufr->cx : 0;
		for (int x = start; x < row->size; x++) {
			if (row->chars[x] == (uint8_t)c) {
				/* Skip past the target if it's not at
				 * the starting position */
				if (x == bufr->cx && sy == bufr->cy)
					continue;
				/* Kill up to and including this char */
				int endx = x + 1;
				int endy = sy;
				editorDeleteRange(bufr, bufr->cx, bufr->cy,
						  endx, endy, 1);
				return;
			}
		}
		sy++;
	}

	editorSetStatusMessage("'%c' not found", c);
}
