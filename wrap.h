#ifndef EMIL_WRAP_H
#define EMIL_WRAP_H 1

#include "emil.h"

/* Screen cache — tracks which screen line each buffer row starts on
 * under word wrap.  Invalidated on any row mutation. */
void invalidateScreenCache(struct buffer *buf);
void buildScreenCache(struct buffer *buf, int screencols);
int getScreenLineForRow(struct buffer *buf, int row, int screencols);

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

#endif
