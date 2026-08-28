/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include "wrap.h"
#include "unicode.h"
#include "util.h"
#include <limits.h>
#include <stdint.h>
#ifdef EMIL_DEBUG_ROW_CACHE
#include <stdio.h>
#include <stdlib.h>
#endif

/* The whole-row walk, factored out so the cache-miss path and the
 * debug check below cannot drift apart. */
static int walkLineWidth(erow *row) {
	int screen_x = 0;
	for (int i = 0; i < row->size;) {
		screen_x = nextScreenX(row->chars, &i, screen_x);
		i++;
	}
	return screen_x;
}

/* Total display width of a row, cached in row->cached_width.
 *
 * EMIL_DEBUG_ROW_CACHE recomputes on every cache hit and aborts on a
 * mismatch.*/
int calculateLineWidth(erow *row) {
	if (row->cached_width >= 0) {
#ifdef EMIL_DEBUG_ROW_CACHE
		int fresh = walkLineWidth(row);
		if (fresh != row->cached_width) {
			fprintf(stderr,
				"emil: stale cached_width on a %d-byte row: "
				"cached %d, actual %d\n",
				row->size, row->cached_width, fresh);
			abort();
		}
#endif
		return row->cached_width;
	}

	row->cached_width = walkLineWidth(row);
	return row->cached_width;
}

/* Display column of a byte offset within a row.
 *
 * char_pos >= row->size means "the whole row", which is  what
 * calculateLineWidth() computes and caches.  The comparison must be
 * >=, not >: a > here sends a cursor at end of line down the
 * O(row->size) walk while the cached answer sits unused.  The
 * frame computes this column once and passes it to the status bar
 * and the cursor placement, so the cache serves repeat frames on an
 * unedited row rather than repeat callers within one frame.*/
int charsToDisplayColumn(erow *row, int char_pos) {
	if (!row || char_pos < 0)
		return 0;
	if (char_pos >= row->size) {
		return calculateLineWidth(row);
	}

	int col = 0;
	for (int i = 0; i < char_pos && i < row->size; i++) {
		col = nextScreenX(row->chars, &i, col);
	}
	return col;
}

/* 行首禁则: a break must not be recorded when the character that
 * would begin the next line is forbidden there (closing punctuation
 * such as 。 」 ）).  Suppressing the candidate makes the break fall
 * earlier, carrying the punctuation to the next line attached to its
 * preceding character; chains (字」。) resolve by suppressing each
 * candidate in turn.  If every candidate on a segment is suppressed,
 * the hard-break fallback still applies, so a pathological line of
 * pure punctuation can neither loop nor produce an empty line. */
static int breakForbiddenBefore(erow *row, int next_bidx) {
	if (next_bidx >= row->size)
		return 0;
	uint8_t c = row->chars[next_bidx];
	if (c < 0x80)
		return 0;
	return isLineStartForbidden(utf8Decode(row->chars, next_bidx));
}

/* Word material for the purpose of the intra-word rule below: an
 * ASCII alphanumeric, or any non-ASCII byte.  Lead and continuation
 * bytes are both >= 0x80, so "café.txt" is treated exactly like
 * "file.txt" without decoding the codepoint. */
static int isWordMaterial(uint8_t c) {
	return ('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') ||
	       ('0' <= c && c <= '9') || c >= 0x80;
}

/* 行末禁则: the mirror of breakForbiddenBefore().  A break must not
 * be recorded when the character that would END the current line is
 * forbidden there.  isWordBoundary() treats nearly every ASCII
 * punctuation mark as a break opportunity, and wordWrapBreak()
 * records the break to its RIGHT, which strands two families of
 * character at the end of a line:
 *
 *   1. Openers ( [ { < and opening quotes.  An opener stranded at
 *      the end of a line is cut off from what it opens.
 *   2. Intra-word punctuation ' . , : — a break opportunity only
 *      when NOT glued to word material on both sides, so "it's",
 *      "file.txt", "1,000" and "12:30" stay whole while ordinary
 *      sentence punctuation ("end. Next", "a, b") still breaks.
 *
 * The ASCII quotes ' and " serve as both opener and closer, so they
 * count as openers exactly when they start the row or follow a space,
 * a tab, or another opener.
 *
 * Suppressing a candidate makes the break fall earlier, carrying the
 * punctuation to the next line attached to what follows it.  If every
 * candidate on a segment is suppressed, the hard-break fallback in
 * wordWrapBreak() still applies, so a pathological line of pure
 * openers ("((((((((") can neither loop nor produce an empty line.
 *
 * This is a wrap-local rule.  isWordBoundary() itself must not change:
 * motion.c depends on its semantics for word movement and transform.c
 * for case transforms. */
static int breakForbiddenAfter(erow *row, int bidx) {
	uint8_t c = row->chars[bidx];

	if (c == '(' || c == '[' || c == '{' || c == '<')
		return 1;

	if (c == '"' || c == '\'') {
		if (bidx == 0)
			return 1;
		uint8_t p = row->chars[bidx - 1];
		if (p == ' ' || p == '\t' || p == '(' || p == '[' || p == '{' ||
		    p == '<')
			return 1;
	}

	if (c == '\'' || c == '.' || c == ',' || c == ':')
		return bidx > 0 && isWordMaterial(row->chars[bidx - 1]) &&
		       bidx + 1 < row->size &&
		       isWordMaterial(row->chars[bidx + 1]);

	return 0;
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
	/* Last hard-break position not immediately after a preposed
	 * vowel (Thai เ แ โ ใ ไ etc.), so the no-word-boundary
	 * fallback never splits a vowel from its consonant. */
	int hard_col = line_start_col;
	int hard_byte = line_start_byte;
	int prev_preposed = 0;

	while (bidx < row->size) {
		if (!prev_preposed) {
			hard_col = col;
			hard_byte = bidx;
		}
		uint8_t c = row->chars[bidx];
		/* Width from THE rule (charAdvance, #117 R1).  The
		 * render loop draws each sub-line with the same rule;
		 * a one-column disagreement between the two shifts
		 * text and moves the cursor off its character, so
		 * they must share the computation, not agree by
		 * inspection. */
		int nb;
		int cwidth = charAdvance(row->chars, bidx, col, &nb);

		/* Wide char won't fit: leave a 1-col gap and break. */
		if (cwidth > 1 && col + cwidth - line_start_col > screencols)
			break;
		if (col + cwidth - line_start_col > screencols)
			break;

		int this_preposed = 0;
		if (isWordBoundary(c)) {
			if (!breakForbiddenAfter(row, bidx) &&
			    !breakForbiddenBefore(row, bidx + nb)) {
				wb_col = col + cwidth;
				wb_byte = bidx + nb;
			}
		} else if (c >= 0x80) {
			uint32_t cp = utf8Decode(row->chars, bidx);
			if ((isCJKChar(cp) || isLineStartForbidden(cp) ||
			     isWordSeparatorCP(cp)) &&
			    !breakForbiddenBefore(row, bidx + nb)) {
				wb_col = col + cwidth;
				wb_byte = bidx + nb;
			}
			this_preposed = isPreposedVowel(cp);
		}
		prev_preposed = this_preposed;

		col += cwidth;
		bidx += nb;
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
	} else if (hard_byte > line_start_byte && hard_byte < bidx) {
		*break_col = hard_col;
		*break_byte = hard_byte;
	} else {
		/* Nothing fit: the segment's first character is wider
		 * than the window.  Emit it anyway.  Returning a break
		 * at line_start_byte would make callers loop forever.
		 * charAdvance, not charInStringWidth: the main loop
		 * above priced this character with tab-stop context
		 * when deciding it didn't fit, and the render loop
		 * will expand a tab to its stop, so break_col must be
		 * charged the same way (a leading tab on a sub-8-col
		 * window was previously charged 2 here). */
		if (bidx == line_start_byte && bidx < row->size) {
			int nb;
			col += charAdvance(row->chars, bidx, col, &nb);
			bidx += nb;
		}
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

void screenWalkStart(struct screenWalk *w, struct buffer *buf, int screencols,
		     int row, int subline) {
	w->buf = buf;
	w->screencols = screencols;
	w->row = row < 0 ? 0 : row;
	if (w->row > buf->numrows - 1)
		w->row = buf->numrows - 1;
	w->subline = 0;
	w->col = 0;
	w->byte = 0;

	if (!buf->word_wrap || subline <= 0)
		return;

	erow *r = &buf->row[w->row];
	while (w->subline < subline) {
		int bc, bb;
		if (!wordWrapBreak(r, screencols, w->col, w->byte, &bc, &bb))
			break; /* subline past the last: clamp */
		w->subline++;
		w->col = bc;
		w->byte = bb;
	}
}

int screenWalkNext(struct screenWalk *w) {
	struct buffer *buf = w->buf;

	if (buf->word_wrap) {
		int bc, bb;
		if (wordWrapBreak(&buf->row[w->row], w->screencols, w->col,
				  w->byte, &bc, &bb)) {
			w->subline++;
			w->col = bc;
			w->byte = bb;
			return 1;
		}
	}

	if (w->row >= buf->numrows - 1)
		return 0;

	w->row++;
	w->subline = 0;
	w->col = 0;
	w->byte = 0;
	return 1;
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

	/* Cursor is past the end */
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
		/* charAdvance, THE width rule (#117 R1, fixes DEF-5).
		 * This walk navigates WITHIN a sub-line whose
		 * boundaries wordWrapBreak defined, so the two must
		 * price every byte identically.  The open-coded rule
		 * here had an `else if (c < 0x80) cwidth = 1` that
		 * caught NUL at 1 column while wordWrapBreak gave it
		 * 2 — unreachable through a buffer (load rejects NUL,
		 * §3.21.1) but exactly the divergence class the
		 * shared rule exists to make impossible. */
		int nb;
		int cwidth = charAdvance(row->chars, bidx, ls_col + col, &nb);

		if (col + cwidth > target_col)
			break;

		col += cwidth;
		bidx += nb;
	}

	return bidx;
}
