#include "wrap.h"
#include "unicode.h"
#include "util.h"
#include <limits.h>
#include <stdint.h>

void invalidateScreenCache(struct buffer *buf) {
	buf->screen_line_cache_valid = 0;
}

void buildScreenCache(struct buffer *buf, int screencols) {
	if (buf->screen_line_cache_valid)
		return;

	if (buf->screen_line_cache_size < buf->numrows) {
		size_t new_size = buf->numrows;
		if (new_size <= SIZE_MAX - 100) {
			new_size += 100;
		}
		if (new_size > SIZE_MAX / sizeof(int)) {
			return;
		}
		buf->screen_line_cache_size = new_size;
		buf->screen_line_start =
			xrealloc(buf->screen_line_start,
				 buf->screen_line_cache_size * sizeof(int));
	}

	if (!buf->screen_line_start)
		return;

	int screen_line = 0;
	for (int i = 0; i < buf->numrows; i++) {
		buf->screen_line_start[i] = screen_line;
		if (!buf->word_wrap) {
			screen_line += 1;
		} else {
			/* Recompute only if cached_width is stale (-1) */
			if (buf->row[i].cached_width < 0) {
				buf->row[i].cached_width =
					calculateLineWidth(&buf->row[i]);
			}
			screen_line +=
				countScreenLines(&buf->row[i], screencols);
		}
	}

	buf->screen_line_cache_valid = 1;
}

int getScreenLineForRow(struct buffer *buf, int row, int screencols) {
	if (!buf->screen_line_cache_valid) {
		buildScreenCache(buf, screencols);
	}
	if (row >= buf->numrows || row < 0)
		return 0;
	return buf->screen_line_start[row];
}

int calculateLineWidth(erow *row) {
	if (row->cached_width >= 0) {
		return row->cached_width;
	}

	int screen_x = 0;
	for (int i = 0; i < row->size;) {
		screen_x = nextScreenX(row->chars, &i, screen_x);
		i++;
	}

	row->cached_width = screen_x;
	return screen_x;
}

int charsToDisplayColumn(erow *row, int char_pos) {
	if (!row || char_pos < 0)
		return 0;
	if (char_pos > row->size) {
		return calculateLineWidth(row);
	}

	int col = 0;
	for (int i = 0; i < char_pos && i < row->size; i++) {
		if (row->chars[i] == '\t') {
			col = (col + EMIL_TAB_STOP) / EMIL_TAB_STOP *
			      EMIL_TAB_STOP;
		} else if (row->chars[i] < 0x20 || row->chars[i] == 0x7f) {
			col += 2;
		} else if (row->chars[i] < 0x80) {
			col += 1;
		} else {
			col += charInStringWidth(row->chars, i);
			i += utf8_nBytes(row->chars[i]) - 1;
		}
	}
	return col;
}

/* Find the next word-wrap break point for a single screen line.
 *
 * Given a row, a screen width, and a starting position (column and byte
 * offset), compute where this screen line ends.  On return, *break_col
 * and *break_byte hold the position just past the last character that
 * fits on this screen line.
 *
 * Returns 1 if more content follows the break (i.e. the row continues
 * onto another screen line), or 0 if the rest of the row fits on this
 * screen line (meaning this is the last sub-line). */
int wordWrapBreak(erow *row, int screencols, int line_start_col,
		  int line_start_byte, int *break_col, int *break_byte) {
	int col = line_start_col;
	int bidx = line_start_byte;
	int wb_col = -1;
	int wb_byte = -1;

	while (bidx < row->size) {
		uint8_t c = row->chars[bidx];
		int cwidth;

		if (c == '\t') {
			cwidth = EMIL_TAB_STOP - (col % EMIL_TAB_STOP);
		} else if (ISCTRL(c)) {
			cwidth = 2;
		} else {
			cwidth = charInStringWidth(row->chars, bidx);
		}

		/* Wide char won't fit: leave a 1-col gap and break. */
		if (cwidth > 1 && col + cwidth - line_start_col > screencols)
			break;
		if (col + cwidth - line_start_col > screencols)
			break;

		if (isWordBoundary(c)) {
			wb_col = col + cwidth;
			wb_byte = bidx + utf8_nBytes(c);
		}

		col += cwidth;
		bidx += utf8_nBytes(c);
	}

	if (bidx >= row->size) {
		/* Rest of row fits on this screen line. */
		*break_col = col;
		*break_byte = row->size;
		return 0;
	} else if (wb_col > line_start_col) {
		/* Break at the last word boundary. */
		*break_col = wb_col;
		*break_byte = wb_byte;
	} else {
		/* No word boundary — hard break at column limit. */
		*break_col = col;
		*break_byte = bidx;
	}
	return 1;
}

/* Count how many screen lines a row occupies under word wrap. */
int countScreenLines(erow *row, int screencols) {
	if (screencols <= 0 || row->size == 0)
		return 1;

	int lines = 0;
	int line_start_col = 0;
	int line_start_byte = 0;

	do {
		int break_col, break_byte;
		int more = wordWrapBreak(row, screencols, line_start_col,
					 line_start_byte, &break_col,
					 &break_byte);
		lines++;
		if (!more)
			break;
		line_start_col = break_col;
		line_start_byte = break_byte;
	} while (line_start_byte < row->size);

	return lines;
}

/* Find which screen line and column a cursor position falls on
 * under word wrap.  Sets *out_line (0-based sub-line within the
 * row) and *out_col (column offset within that sub-line). */
void cursorScreenLine(erow *row, int cursor_col, int screencols, int *out_line,
		      int *out_col) {
	*out_line = 0;
	*out_col = 0;

	if (screencols <= 0 || row->size == 0) {
		*out_col = cursor_col;
		return;
	}

	int line_start_col = 0;
	int line_start_byte = 0;

	while (line_start_byte < row->size) {
		int break_col, break_byte;
		int more = wordWrapBreak(row, screencols, line_start_col,
					 line_start_byte, &break_col,
					 &break_byte);

		/* cursor_col falls within this screen line */
		if (cursor_col < break_col || !more) {
			*out_col = cursor_col - line_start_col;
			return;
		}

		(*out_line)++;
		line_start_col = break_col;
		line_start_byte = break_byte;
	}

	/* Cursor is past the end — place on the last sub-line */
	*out_col = cursor_col - line_start_col;
}

/* Find the byte-offset boundaries of a given sub-line within a wrapped row.
 * Sets *start_byte and *end_byte.  end_byte is the first byte of the
 * next sub-line (or row->size for the last sub-line).
 * Returns 0 if target_subline is beyond the row's sub-lines. */
int sublineBounds(erow *row, int screencols, int target_subline,
		  int *start_byte, int *end_byte) {
	int ls_col = 0, ls_byte = 0;

	for (int sl = 0; sl < target_subline; sl++) {
		int break_col, break_byte;
		int more = wordWrapBreak(row, screencols, ls_col, ls_byte,
					 &break_col, &break_byte);
		if (!more) {
			/* target_subline doesn't exist */
			*start_byte = row->size;
			*end_byte = row->size;
			return 0;
		}
		ls_col = break_col;
		ls_byte = break_byte;
	}

	*start_byte = ls_byte;

	/* Find end of this sub-line */
	int break_col, break_byte;
	int more = wordWrapBreak(row, screencols, ls_col, ls_byte, &break_col,
				 &break_byte);
	if (!more)
		*end_byte = row->size;
	else
		*end_byte = break_byte;

	return 1;
}

/* Given a sub-line number and a display column within that sub-line,
 * return the byte offset in row->chars closest to that position.
 * If target_subline is past the end of the row, returns row->size. */
int displayColumnToByteOffset(erow *row, int screencols, int target_subline,
			      int target_col) {
	if (!row || row->size == 0)
		return 0;

	/* Phase 1: find the start of the target sub-line */
	int ls_col = 0, ls_byte = 0;

	for (int sl = 0; sl < target_subline; sl++) {
		int break_col, break_byte;
		int more = wordWrapBreak(row, screencols, ls_col, ls_byte,
					 &break_col, &break_byte);
		if (!more) {
			/* target sub-line doesn't exist */
			return row->size;
		}
		ls_col = break_col;
		ls_byte = break_byte;
	}

	/* Find end of this sub-line for clamping */
	int end_col, end_byte;
	int more = wordWrapBreak(row, screencols, ls_col, ls_byte, &end_col,
				 &end_byte);
	int subline_end_byte = more ? end_byte : row->size;

	/* Phase 2: walk the sub-line to find the target column */
	int col = 0; /* column relative to sub-line start */
	int bidx = ls_byte;

	while (bidx < subline_end_byte) {
		uint8_t c = row->chars[bidx];
		int cwidth;

		if (c == '\t') {
			int abs_col = ls_col + col;
			cwidth = EMIL_TAB_STOP - (abs_col % EMIL_TAB_STOP);
		} else if (ISCTRL(c)) {
			cwidth = 2;
		} else if (c < 0x80) {
			cwidth = 1;
		} else {
			cwidth = charInStringWidth(row->chars, bidx);
		}

		if (col + cwidth > target_col)
			break;

		col += cwidth;
		bidx += utf8_nBytes(c);
	}

	return bidx;
}
