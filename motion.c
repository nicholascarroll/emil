#include "motion.h"
#include "display.h"
#include "message.h"
#include "prompt.h"
#include "region.h"
#include "unicode.h"
#include "util.h"
#include "window.h"
#include "wrap.h"
#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>

extern struct config E;

/* Boundary detection */

int isParaBoundary(erow *row) {
	return (row->size == 0);
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
int bufferForwardSexpEnd(int *cx, int *cy, const char **errmsg) {
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

/* Page/scroll navigation */

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

static int is_punct(char c) {
	return c == '.' || c == '!' || c == '?';
}

/* Returns 1 if 'x' is the index of the Uppercase letter starting a sentence */
int isSentenceBoundary(erow *row, int x) {
	if (x < 2)
		return 0;
	return (is_punct(row->chars[x - 2]) && row->chars[x - 1] == ' ' &&
		isupper((unsigned char)row->chars[x])) ||
	       (is_punct(row->chars[x - 3]) && row->chars[x - 2] == ' ' &&
		row->chars[x - 1] == ' ' &&
		isupper((unsigned char)row->chars[x]));
}

/**
 * Moves forward to the next sentence end.
 * Boundary: The space after punctuation OR the end of the line.
 */
int forwardSentenceEnd(int *cx, int *cy) {
	int start_x = *cx, start_y = *cy;

	for (int y = start_y; y < E.buf->numrows; y++) {
		erow *row = &E.buf->row[y];
		int x = (y == start_y) ? start_x : 0;

		for (; x < row->size; x++) {
			// Check for [Punct][Space][Upper] pattern
			if (is_punct(row->chars[x]) &&
			    (isSentenceBoundary(row, x + 2) ||
			     isSentenceBoundary(row, x + 3))) {
				int pot_x = x + 1; // Target is the space
				if (y > start_y || pot_x > start_x) {
					*cx = pot_x;
					*cy = y;
					return 0;
				}
			}
		}

		// Invariant: End of line is always a sentence boundary
		if (y > start_y || row->size > start_x) {
			*cx = row->size;
			*cy = y;
			return 0;
		}
	}
	return -1;
}

/**
 * Moves backward to the nearest sentence start.
 * Boundary: The Uppercase letter of a pattern OR the start of a line (index 0).
 */
int backwardSentenceStart(int *cx, int *cy) {
	int start_x = *cx, start_y = *cy;

	for (int y = start_y; y >= 0; y--) {
		erow *row = &E.buf->row[y];

		// Start from current x on the first line, otherwise start from the end
		int x = (y == start_y) ? start_x : row->size;

		for (int i = x; i >= 0; i--) {
			// Empty lines are boundaries
			if (row->size == 0) {
				if (y < start_y || 0 < start_x) {
					*cx = 0;
					*cy = y;
					return 0;
				}
			}

			// Pattern match or Start of Line (index 0)
			if (i == 0 || isSentenceBoundary(row, i)) {
				if (y < start_y || i < start_x) {
					*cx = i;
					*cy = y;
					return 0;
				}
			}
		}
	}
	return -1;
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
