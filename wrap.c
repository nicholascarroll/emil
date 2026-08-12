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

	/* A column-width change invalidates every row's cached subline
	 * count (the wrap points move even though the text didn't). */
	if (buf->screen_cache_cols != screencols) {
		for (int i = 0; i < buf->numrows; i++)
			buf->row[i].cached_sublines = -1;
		buf->screen_cache_cols = screencols;
	}

	int screen_line = 0;
	for (int i = 0; i < buf->numrows; i++) {
		buf->screen_line_start[i] = screen_line;
		if (!buf->word_wrap) {
			screen_line += 1;
		} else {
			/* Recompute the subline count only for rows
			 * marked stale (-1) by a mutation site.
			 *
			 * The width-stale check below is a safety net
			 * only, NOT the invalidation mechanism: it
			 * cannot be relied on because
			 * calculateLineWidth() (called from display
			 * paths) may re-validate cached_width before
			 * this rebuild runs, hiding the staleness.
			 * The real invariant lives at the mutation
			 * sites: every one must set BOTH cached_width
			 * and cached_sublines to -1 (see erow in
			 * emil.h). */
			if (buf->row[i].cached_width < 0) {
				buf->row[i].cached_width =
					calculateLineWidth(&buf->row[i]);
				buf->row[i].cached_sublines = -1;
			}
			if (buf->row[i].cached_sublines < 0)
				buf->row[i].cached_sublines = countScreenLines(
					&buf->row[i], screencols);
			screen_line += buf->row[i].cached_sublines;
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

/* The whole-row walk, factored out so the cache-miss path and the
 * debug check below cannot drift apart.  A check computing the value
 * a second way would eventually disagree for its own reasons and be
 * disbelieved. */
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
 * mismatch.  §4.10 obliges every mutation site to set cached_width to
 * -1 by hand; there is no mechanical check (Appendix C.2), and
 * charsToDisplayColumn() now routes char_pos >= row->size here, which
 * put two per-frame drawRows() callers on the cached path that were
 * walking the row fresh before.  A missed invalidation used to show up
 * only on the rare char_pos > row->size path; now it renders wrongly
 * every frame.  That is a good trade -- it is the whole point of the
 * change -- but it wants a net.
 *
 * Not gated on NDEBUG, which the Makefile never defines, so it would
 * be on in the build users get.  The check IS the walk the cache
 * exists to avoid.  Measured on a 50 MB single line at 80 columns
 * (gcc 13 -O2), the four charsToDisplayColumn(row, row->size) calls
 * drawRows() makes on a non-editing frame: 409 ms before this change,
 * 0.00 ms after it, 439 ms after it with the check on.  An always-on
 * check would not dilute this change, it would reverse it.  The
 * sanitize target defines the macro instead, which is what the
 * pre-merge run uses. */
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
 * char_pos >= row->size means "the whole row", which is exactly what
 * calculateLineWidth() computes and caches.  The comparison must be
 * >=, not >: the two hot callers in drawRows() pass row->size itself,
 * and a > here sent them down the O(row->size) walk on every frame
 * while the cached answer sat unused.  On a 50 MB line that was 53 ms
 * per call against 0.0001 ms cached.
 *
 * The walk delegates to nextScreenX() rather than repeating its width
 * rules.  An earlier copy here drifted from it and was the reason two
 * paths could disagree about a tab or a wide character. */
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

		int this_preposed = 0;
		if (isWordBoundary(c)) {
			if (!breakForbiddenAfter(row, bidx) &&
			    !breakForbiddenBefore(row, bidx + utf8_nBytes(c))) {
				wb_col = col + cwidth;
				wb_byte = bidx + utf8_nBytes(c);
			}
		} else if (c >= 0x80) {
			uint32_t cp = utf8Decode(row->chars, bidx);
			if ((isCJKChar(cp) || isLineStartForbidden(cp) ||
			     isWordSeparatorCP(cp)) &&
			    !breakForbiddenBefore(row, bidx + utf8_nBytes(c))) {
				wb_col = col + cwidth;
				wb_byte = bidx + utf8_nBytes(c);
			}
			this_preposed = isPreposedVowel(cp);
		}
		prev_preposed = this_preposed;

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
	} else if (hard_byte > line_start_byte && hard_byte < bidx) {
		*break_col = hard_col;
		*break_byte = hard_byte;
	} else {
		/* Nothing fit: the segment's first character is wider
		 * than the window.  Emit it anyway.  Returning a break
		 * at line_start_byte would make callers loop forever. */
		if (bidx == line_start_byte && bidx < row->size) {
			col += charInStringWidth(row->chars, bidx);
			bidx += utf8_nBytes(row->chars[bidx]);
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
