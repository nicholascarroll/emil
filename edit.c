#include "edit.h"
#include "adjust.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "history.h"
#include "keymap.h"
#include "message.h"
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

void insertNewlineRaw(void) {
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
		insertNewlineRaw();
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
	struct undo *new = newUndo();
	new->startx = 0;
	new->starty = E.buf->cy;
	new->endx = trunc;
	new->endy = E.buf->cy;
	new->delete = 1;
	new->append = 0;
	pushUndo(E.buf, new);
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
	E.buf->cx -= trunc;
	row->cached_width = -1;
	invalidateScreenCache(E.buf);
	E.buf->dirty = 1;

	/* Adjust tracked points for this deletion */
	adjustAllPoints(E.buf, 0, E.buf->cy, trunc, E.buf->cy, 1);
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

/* Boundary detection */

int isParaBoundary(erow *row) {
	return (row->size == 0);
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

/* Cursor movement */

/* Move cursor up or down by one visual (screen) row when word wrap is
 * active.  direction: -1 = up, +1 = down. */
static void moveVisualRow(int direction) {
	if (E.buf->cy >= E.buf->numrows) {
		if (direction > 0 || E.buf->numrows == 0)
			return;
		/* Moving up from virtual EOF line */
		E.buf->cy = E.buf->numrows - 1;
		erow *prev = &E.buf->row[E.buf->cy];
		int last_sub = countScreenLines(prev, E.screencols) - 1;
		E.buf->cx = displayColumnToByteOffset(prev, E.screencols,
						      last_sub, 0);
		return;
	}

	erow *row = &E.buf->row[E.buf->cy];
	int display_col = charsToDisplayColumn(row, E.buf->cx);

	int current_subline, sub_col;
	cursorScreenLine(row, display_col, E.screencols, &current_subline,
			 &sub_col);

	int target_subline = current_subline + direction;
	int total_sublines = countScreenLines(row, E.screencols);

	if (target_subline >= 0 && target_subline < total_sublines) {
		/* Move within the same logical row */
		E.buf->cx = displayColumnToByteOffset(row, E.screencols,
						      target_subline, sub_col);
	} else if (target_subline < 0) {
		/* Move to previous logical row */
		if (E.buf->cy == 0)
			return;
		E.buf->cy--;
		erow *prev = &E.buf->row[E.buf->cy];
		int last_sub = countScreenLines(prev, E.screencols) - 1;
		E.buf->cx = displayColumnToByteOffset(prev, E.screencols,
						      last_sub, sub_col);
	} else {
		/* Move to next logical row */
		if (E.buf->cy >= E.buf->numrows - 1) {
			/* Allow moving to the virtual line past EOF */
			E.buf->cy = E.buf->numrows;
			E.buf->cx = 0;
			return;
		}
		E.buf->cy++;
		erow *next = &E.buf->row[E.buf->cy];
		E.buf->cx = displayColumnToByteOffset(next, E.screencols, 0,
						      sub_col);
	}
}

void moveCursor(int key, int count) {
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
				moveVisualRow(-1);
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
				moveVisualRow(+1);
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

void forwardWordEnd(int *dx, int *dy) {
	int cx = E.buf->cx;
	int icy = E.buf->cy;
	if (icy >= E.buf->numrows) {
		*dx = cx;
		*dy = icy;
		return;
	}
	int pre = 1;
	for (int cy = icy; cy < E.buf->numrows; cy++) {
		int l = E.buf->row[cy].size;
		while (cx < l) {
			uint8_t c = E.buf->row[cy].chars[cx];
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

void backwardWordEnd(int *dx, int *dy) {
	int cx = E.buf->cx;
	int icy = E.buf->cy;

	if (icy >= E.buf->numrows) {
		return;
	}

	int pre = 1;

	for (int cy = icy; cy >= 0; cy--) {
		if (cy != icy) {
			cx = E.buf->row[cy].size;
		}
		while (cx > 0) {
			uint8_t c = E.buf->row[cy].chars[cx - 1];
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

void forwardWord(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		forwardWordEnd(&E.buf->cx, &E.buf->cy);
	}
}

void backWord(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		backwardWordEnd(&E.buf->cx, &E.buf->cy);
	}
}

/* Paragraph movement */

/* Buffer-parameterized helpers for paragraph boundary scanning.
 * These set cx=0 and update *cy to the boundary line. */

void backwardParaBoundary(int *cx, int *cy) {
	*cx = 0;
	int icy = *cy;

	if (icy >= E.buf->numrows) {
		icy--;
	}

	if (E.buf->numrows == 0) {
		return;
	}

	int pre = 1;

	for (int y = icy; y >= 0; y--) {
		erow *row = &E.buf->row[y];
		if (isParaBoundary(row) && !pre) {
			*cy = y;
			return;
		} else if (!isParaBoundary(row)) {
			pre = 0;
		}
	}

	*cy = 0;
}

void forwardParaBoundary(int *cx, int *cy) {
	*cx = 0;
	int icy = *cy;

	if (icy >= E.buf->numrows) {
		return;
	}

	if (E.buf->numrows == 0) {
		return;
	}

	int pre = 1;

	for (int y = icy; y < E.buf->numrows; y++) {
		erow *row = &E.buf->row[y];
		if (isParaBoundary(row) && !pre) {
			*cy = y;
			return;
		} else if (!isParaBoundary(row)) {
			pre = 0;
		}
	}

	*cy = E.buf->numrows;
}

void backPara(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		backwardParaBoundary(&E.buf->cx, &E.buf->cy);
	}
}

void forwardPara(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		forwardParaBoundary(&E.buf->cx, &E.buf->cy);
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
static int stepForward(int *cx, int *cy) {
	if (*cy >= E.buf->numrows)
		return 0;
	if (*cx < E.buf->row[*cy].size) {
		(*cx)++;
		return 1;
	}
	if (*cy + 1 < E.buf->numrows) {
		*cy += 1;
		*cx = 0;
		return 1;
	}
	return 0;
}

/* Retreat one position backward in the buffer.  Returns 0 at start. */
static int stepBackward(int *cx, int *cy) {
	if (*cx > 0) {
		(*cx)--;
		return 1;
	}
	if (*cy > 0) {
		*cy -= 1;
		*cx = E.buf->row[*cy].size;
		return 1;
	}
	return 0;
}

/* Get the character at (cx, cy).  Returns 0 at end-of-line / end-of-buffer.
 * End-of-line positions (cx == row->size) are treated as newline. */
static uint8_t charAt(int cx, int cy) {
	if (cy >= E.buf->numrows)
		return 0;
	erow *row = &E.buf->row[cy];
	if (cx >= row->size)
		return '\n';
	return row->chars[cx];
}

/* Scan forward from (*cx, *cy) past one sexp (balanced expression).
 * On success, updates (*cx, *cy) to just past the sexp and returns 0.
 * On failure (unmatched delimiter, end of buffer), returns -1 without
 * modifying *cx / *cy; *errmsg is set to a description. */
static int bufferForwardSexpEnd(int *cx, int *cy, const char **errmsg) {
	int px = *cx, py = *cy;

	/* Skip whitespace and newlines */
	while (py < E.buf->numrows) {
		uint8_t ch = charAt(px, py);
		if (ch == 0) {
			*errmsg = "End of buffer";
			return -1;
		}
		if (ch != ' ' && ch != '\t' && ch != '\n')
			break;
		stepForward(&px, &py);
	}
	if (py >= E.buf->numrows) {
		*errmsg = "End of buffer";
		return -1;
	}

	uint8_t ch = charAt(px, py);

	/* Opening delimiter: scan forward for matching close */
	int close = matchingClose(ch);
	if (close) {
		int depth = 1;
		int sx = px, sy = py;
		stepForward(&sx, &sy);
		while (depth > 0) {
			uint8_t c = charAt(sx, sy);
			if (c == 0) {
				*errmsg = "Unmatched delimiter";
				return -1;
			}
			if ((int)c == close)
				depth--;
			else if (c == ch)
				depth++;
			if (depth > 0)
				stepForward(&sx, &sy);
		}
		/* Land after the closing delimiter */
		stepForward(&sx, &sy);
		*cx = sx;
		*cy = sy;
		return 0;
	}

	/* Closing delimiter while inside: jump past it */
	if (matchingOpen(ch)) {
		stepForward(&px, &py);
		*cx = px;
		*cy = py;
		return 0;
	}

	/* Quote character: scan forward for matching quote */
	if (isQuoteChar(ch)) {
		int sx = px, sy = py;
		stepForward(&sx, &sy);
		while (1) {
			uint8_t c = charAt(sx, sy);
			if (c == 0) {
				*errmsg = "Unmatched quote";
				return -1;
			}
			if (c == ch) {
				stepForward(&sx, &sy);
				*cx = sx;
				*cy = sy;
				return 0;
			}
			stepForward(&sx, &sy);
		}
	}

	/* Word: skip to end of word */
	*cx = px;
	*cy = py;
	forwardWordEnd(cx, cy);
	return 0;
}

void forwardSexp(int count) {
	int times = count ? count : 1;

	for (int t = 0; t < times; t++) {
		int cx = E.buf->cx;
		int cy = E.buf->cy;
		const char *errmsg = NULL;

		if (bufferForwardSexpEnd(&cx, &cy, &errmsg) < 0) {
			setStatusMessage("%s", errmsg);
			return;
		}
		E.buf->cx = cx;
		E.buf->cy = cy;
	}
}

void backwardSexp(int count) {
	int times = count ? count : 1;

	for (int t = 0; t < times; t++) {
		int cx = E.buf->cx;
		int cy = E.buf->cy;

		/* Step back once then skip whitespace and newlines */
		if (!stepBackward(&cx, &cy)) {
			setStatusMessage(msg_beginning_of_buffer);
			return;
		}
		while (1) {
			uint8_t ch = charAt(cx, cy);
			if (ch != ' ' && ch != '\t' && ch != '\n')
				break;
			if (!stepBackward(&cx, &cy)) {
				setStatusMessage(msg_beginning_of_buffer);
				return;
			}
		}

		uint8_t ch = charAt(cx, cy);

		/* Closing delimiter: scan backward for matching open */
		int open = matchingOpen(ch);
		if (open) {
			int depth = 1;
			int sx = cx, sy = cy;
			while (depth > 0) {
				if (!stepBackward(&sx, &sy)) {
					setStatusMessage("Unmatched delimiter");
					return;
				}
				uint8_t c = charAt(sx, sy);
				if ((int)c == open)
					depth--;
				else if (c == ch)
					depth++;
			}
			E.buf->cx = sx;
			E.buf->cy = sy;
			continue;
		}

		/* Opening delimiter while inside: land on it */
		if (matchingClose(ch)) {
			E.buf->cx = cx;
			E.buf->cy = cy;
			continue;
		}

		/* Quote character: scan backward for matching quote */
		if (isQuoteChar(ch)) {
			int sx = cx, sy = cy;
			while (1) {
				if (!stepBackward(&sx, &sy)) {
					setStatusMessage("Unmatched quote");
					return;
				}
				uint8_t c = charAt(sx, sy);
				if (c == ch) {
					E.buf->cx = sx;
					E.buf->cy = sy;
					break;
				}
			}
			continue;
		}

		/* Word: skip to beginning of word */
		E.buf->cx = cx;
		E.buf->cy = cy;
		/* stepForward to undo the stepBackward, then use word movement */
		stepForward(&E.buf->cx, &E.buf->cy);
		backwardWordEnd(&E.buf->cx, &E.buf->cy);
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

/* Navigation */

void pageUp(int count) {
	struct window *win = E.windows[windowFocusedIdx()];
	int times = count ? count : 1;

	for (int n = 0; n < times; n++) {
		int scroll_lines = win->height - page_overlap;
		if (scroll_lines < 1)
			scroll_lines = 1;

		scrollViewport(win, E.buf, -scroll_lines);
		clampCursorToViewport(win, E.buf);
	}
}

void pageDown(int count) {
	struct window *win = E.windows[windowFocusedIdx()];
	int times = count ? count : 1;

	for (int n = 0; n < times; n++) {
		int scroll_lines = win->height - page_overlap;
		if (scroll_lines < 1)
			scroll_lines = 1;

		scrollViewport(win, E.buf, scroll_lines);
		clampCursorToViewport(win, E.buf);
	}
}

void scrollLineUp(int count) {
	struct window *win = E.windows[windowFocusedIdx()];
	int times = count ? count : 1;

	scrollViewport(win, E.buf, -times);
	clampCursorToViewport(win, E.buf);
}

void scrollLineDown(int count) {
	struct window *win = E.windows[windowFocusedIdx()];
	int times = count ? count : 1;

	scrollViewport(win, E.buf, times);
	clampCursorToViewport(win, E.buf);
}

void beginningOfLine(void) {
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

void endOfLine(int count) {
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

void gotoLine(void) {
	setMarkSilent();
	uint8_t *nls = editorPrompt(E.buf, "Goto line: %s", PROMPT_BASIC, NULL);
	if (!nls)
		return;

	int nl = atoi((char *)nls);
	free(nls);

	if (nl == 0)
		return;

	E.buf->cx = 0;
	if (nl < 0) {
		E.buf->cy = 0;
	} else if (nl > E.buf->numrows) {
		E.buf->cy = E.buf->numrows;
	} else {
		E.buf->cy = nl - 1;
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
int forwardSentenceEnd(int *cx, int *cy) {
	int px = *cx, py = *cy;

	while (py < E.buf->numrows) {
		erow *row = &E.buf->row[py];

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
					} else if (py + 1 < E.buf->numrows) {
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
		if (py + 1 < E.buf->numrows &&
		    isParaBoundary(&E.buf->row[py + 1])) {
			/* Sentence ends at end of this line */
			*cx = row->size;
			*cy = py;
			/* Advance past the blank line(s) to match Emacs
			 * behavior: land on first non-blank line */
			py++;
			while (py < E.buf->numrows &&
			       isParaBoundary(&E.buf->row[py]))
				py++;
			*cx = 0;
			*cy = py < E.buf->numrows ? py : E.buf->numrows;
			return 0;
		}

		py++;
		px = 0;
	}

	/* Reached end of buffer */
	*cx = 0;
	*cy = E.buf->numrows;
	return -1;
}

/* Scan backward from (*cx, *cy) to the beginning of the current sentence.
 * Returns 0 on success, -1 at beginning-of-buffer. */
int backwardSentenceStart(int *cx, int *cy) {
	int px = *cx, py = *cy;

	if (py >= E.buf->numrows) {
		if (E.buf->numrows == 0)
			return -1;
		py = E.buf->numrows - 1;
		px = E.buf->row[py].size;
	}

	/* Step back once to avoid detecting the current position */
	if (!stepBackward(&px, &py))
		return -1;

	/* Skip whitespace/newlines backward */
	while (1) {
		uint8_t c = charAt(px, py);
		if (c != ' ' && c != '\t' && c != '\n')
			break;
		if (!stepBackward(&px, &py)) {
			*cx = 0;
			*cy = 0;
			return 0;
		}
	}

	/* Skip closing punctuation and sentence terminators backward
	 * so we don't re-detect the end of the sentence we're at. */
	while (1) {
		uint8_t c = charAt(px, py);
		if (!isSentenceEnd(c) && !isClosingPunct(c))
			break;
		if (!stepBackward(&px, &py)) {
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
		if (py < E.buf->numrows && isParaBoundary(&E.buf->row[py])) {
			/* Land on the first non-blank line after */
			int ny = py;
			while (ny < E.buf->numrows &&
			       isParaBoundary(&E.buf->row[ny]))
				ny++;
			*cx = 0;
			*cy = ny < E.buf->numrows ? ny : py;
			return 0;
		}

		uint8_t c = charAt(px, py);
		if (isSentenceEnd(c)) {
			/* Found a sentence terminator — sentence starts
			 * after this terminator plus any closing punct
			 * and one whitespace char */
			int sx = px, sy = py;
			stepForward(&sx, &sy);
			/* Skip closing punctuation */
			while (sy < E.buf->numrows) {
				uint8_t nc = charAt(sx, sy);
				if (!isClosingPunct(nc))
					break;
				stepForward(&sx, &sy);
			}
			/* Skip one whitespace */
			if (sy < E.buf->numrows) {
				uint8_t nc = charAt(sx, sy);
				if (nc == ' ' || nc == '\t' || nc == '\n')
					stepForward(&sx, &sy);
			}
			*cx = sx;
			*cy = sy;
			return 0;
		}

		if (!stepBackward(&px, &py)) {
			*cx = 0;
			*cy = 0;
			return 0;
		}
	}
}

void forwardSentence(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		forwardSentenceEnd(&E.buf->cx, &E.buf->cy);
	}
}

void backwardSentence(int count) {
	int times = count ? count : 1;
	for (int i = 0; i < times; i++) {
		backwardSentenceStart(&E.buf->cx, &E.buf->cy);
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
			stepForward(&tx, &ty);
		}
		a_text = xmalloc(a_len + 1);
		tx = sx;
		ty = sy;
		for (int i = 0; i < a_len; i++) {
			a_text[i] = charAt(tx, ty);
			stepForward(&tx, &ty);
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
			stepForward(&tx, &ty);
		}
		b_text = xmalloc(b_len + 1);
		tx = sx;
		ty = sy;
		for (int i = 0; i < b_len; i++) {
			b_text[i] = charAt(tx, ty);
			stepForward(&tx, &ty);
		}
		b_text[b_len] = '\0';
	}

	/* Delete the entire range from A start to B end */
	deleteRange(a_start_x, a_start_y, b_end_x, b_end_y, 0);

	/* Insert B text then A text at the deletion point */
	E.buf->cx = a_start_x;
	E.buf->cy = a_start_y;
	for (int i = 0; i < b_len; i++) {
		if (b_text[i] == '\n') {
			insertNewlineRaw();
		} else {
			insertChar(E.buf, (uint8_t)b_text[i], 1);
		}
	}
	for (int i = 0; i < a_len; i++) {
		if (a_text[i] == '\n') {
			insertNewlineRaw();
		} else {
			insertChar(E.buf, (uint8_t)a_text[i], 1);
		}
	}

	free(a_text);
	free(b_text);
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
