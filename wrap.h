/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_WRAP_H
#define EMIL_WRAP_H 1

#include "emil.h"

/* Row geometry — pure computation on erow data. */
int calculateLineWidth(erow *row);
int charsToDisplayColumn(erow *row, int char_pos);
int countScreenLines(erow *row, int screencols);

/* Word-wrap break point for a single screen line.  Returns 1 if more
 * content follows the break, 0 if this is the last sub-line. */
int wordWrapBreak(erow *row, int screencols, int line_start_col,
		  int line_start_byte, int *break_col, int *break_byte);

/* Find which sub-line and column a cursor position falls on. */
void cursorScreenLine(erow *row, int cursor_col, int screencols, int *out_line,
		      int *out_col);

/* Byte-offset boundaries of a given sub-line within a wrapped row. */
int sublineBounds(erow *row, int screencols, int target_subline,
		  int *start_byte, int *end_byte);

/* Byte offset closest to a display column on a given sub-line. */
int displayColumnToByteOffset(erow *row, int screencols, int target_subline,
			      int target_col);

/* A cursor over screen lines, walked forward from a starting position.
 *
 * It carries the wrap state within the current row, so advancing costs
 * one wordWrapBreak() rather than a re-walk from the row's start, and a
 * walk bounded to the window's height touches only the bytes the window
 * shows.  Advancing never asks a row for its total sub-line count,
 * which is what makes a partially visible row cost only its visible
 * part.
 *
 * That is a claim about screenWalkNext(), not about viewport arithmetic
 * generally.  Two costs sit outside it and are proportional to content
 * rather than to the window: screenWalkStart() below, and linesBack()
 * in display.c, which needs a row's LAST sub-line index and can only
 * get it by wrapping the whole row.  A frame whose cursor sits deep
 * inside one very long row therefore still scales with that depth. */
struct screenWalk {
	struct buffer *buf;
	int screencols;
	int row;     /* logical row */
	int subline; /* sub-line index within that row */
	int col;     /* display column at the start of this sub-line */
	int byte;    /* byte offset at the start of this sub-line */
};

/* Position a walk at (row, subline).  Costs the bytes before that
 * sub-line within its row; free when wrap is off.  A subline past the
 * row's last is clamped to it. */
void screenWalkStart(struct screenWalk *w, struct buffer *buf, int screencols,
		     int row, int subline);

/* Advance one screen line.  Returns 0 if the buffer ends here, leaving
 * the walk on its last screen line. */
int screenWalkNext(struct screenWalk *w);

#endif
