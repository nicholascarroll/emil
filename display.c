/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include "display.h"
#include "abuf.h"
#include "buffer.h"
#include "emil.h"
#include "fileio.h"
#include "history.h"

#include "region.h"
#include "wrap.h"
#include "terminal.h"
#include "unicode.h"
#include "util.h"
#include "window.h"
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __sun
#include <termios.h>
#endif

int minibuffer_height = 1;
const int statusbar_height = 1;

/* Pre-computed highlight bounds for a single row.  Computed once per row
 * then checked with simple integer comparisons.
 */
struct rowHighlight {
	int region_start; /* first highlighted display column, or -1 */
	int region_end;	  /* one past last highlighted column, or -1 */
	int match_start;  /* search match start column, or -1 */
	int match_end;	  /* search match end column, or -1 */
};

/* max_col bounds a highlight that runs to end of line.  The bound is
 * the viewport's, not the row's: a region ending past the last column
 * the window can draw is indistinguishable from one ending at it, and
 * asking the row for its total width is an O(row length) walk. */
static void computeRowHighlightBounds(struct buffer *buf, int filerow,
				      int max_col, struct rowHighlight *hl) {
	hl->region_start = -1;
	hl->region_end = -1;
	hl->match_start = -1;
	hl->match_end = -1;

	erow *row = &buf->row[filerow];

	/* Completions buffer: highlight the basename portion of the
	 * currently selected match row only.  buf->cy tracks the
	 * selected row (set by cycleCompletion / showCompletionsBuffer). */
	if (buf->special_buffer && buf->filename &&
	    strcmp(buf->filename, "*Completions*") == 0 && filerow >= 2 &&
	    filerow == buf->cy) {
		/* Find basename: byte offset after last '/' */
		int base_byte = 0;
		for (int i = 0; i < row->size; i++) {
			if (row->chars[i] == '/')
				base_byte = i + 1;
		}
		hl->region_start = charsToDisplayColumn(row, base_byte);
		hl->region_end = max_col;
		return;
	}

	/* markInvalidBuf(buf), not markInvalidSilent(): this function is
	 * called for every window, so 'buf' is frequently not E.buf.  The
	 * no-arg form asked whether the *focused* buffer's mark was valid
	 * before drawing an unfocused buffer's selection. */
	if (buf->mark_active && !markInvalidBuf(buf)) {
		if (buf->rectangle_mode) {
			int top = buf->cy < buf->marky ? buf->cy : buf->marky;
			int bot = buf->cy > buf->marky ? buf->cy : buf->marky;
			if (filerow >= top && filerow <= bot) {
				int left = buf->cx < buf->markx ? buf->cx :
								  buf->markx;
				int right = buf->cx > buf->markx ? buf->cx :
								   buf->markx;
				hl->region_start =
					charsToDisplayColumn(row, left);
				hl->region_end =
					charsToDisplayColumn(row, right);
			}
		} else {
			int sr = buf->cy < buf->marky ? buf->cy : buf->marky;
			int er = buf->cy > buf->marky ? buf->cy : buf->marky;
			if (filerow >= sr && filerow <= er) {
				int sc = (buf->cy < buf->marky ||
					  (buf->cy == buf->marky &&
					   buf->cx <= buf->markx)) ?
						 buf->cx :
						 buf->markx;
				int ec = (buf->cy > buf->marky ||
					  (buf->cy == buf->marky &&
					   buf->cx >= buf->markx)) ?
						 buf->cx :
						 buf->markx;
				if (filerow == sr && filerow == er) {
					hl->region_start =
						charsToDisplayColumn(row, sc);
					hl->region_end =
						charsToDisplayColumn(row, ec);
				} else if (filerow == sr) {
					hl->region_start =
						charsToDisplayColumn(row, sc);
					hl->region_end = max_col;
				} else if (filerow == er) {
					hl->region_start = 0;
					hl->region_end =
						charsToDisplayColumn(row, ec);
				} else {
					/* Middle row: entire row highlighted */
					hl->region_start = 0;
					hl->region_end = max_col;
				}
			}
		}
	}

	/* Search match bounds */
	if (buf->query && buf->query[0] && buf->match && filerow == buf->cy) {
		/* Length of the text actually matched.  For a regex
		 * this differs from the pattern length; fall back to
		 * the pattern length if it was never recorded. */
		int match_len = buf->match_len > 0 ?
					buf->match_len :
					(int)strlen((char *)buf->query);
		hl->match_start = charsToDisplayColumn(row, buf->cx);
		hl->match_end = charsToDisplayColumn(row, buf->cx + match_len);
	}
}

/* Check whether a display column is highlighted, using pre-computed bounds. */
static inline int isHighlighted(const struct rowHighlight *hl, int col) {
	return (col >= hl->region_start && col < hl->region_end) ||
	       (col >= hl->match_start && col < hl->match_end);
}

/* Update highlight state, emitting escape sequences only on transitions */
static void updateHighlight(struct abuf *ab, int *current, int desired) {
	if (desired != *current) {
		if (*current)
			abAppend(ab, "\x1b[0m", 4);
		if (desired)
			abAppend(ab, "\x1b[7m", 4);
		*current = desired;
	}
}

/* ---- Viewport arithmetic, in screen lines relative to the top ----
 *
 * Every viewport question this file asks is a difference or a
 * comparison, and every one of them is bounded by the window height in
 * SCREEN LINES: a row contributes at least one screen line, so a
 * position more than `height` lines from the top is off screen and the
 * answer is "not visible" rather than a number.
 *
 * The top itself is (rowoff, skip_sublines):  a row and a sub-line
 * within it.  These four helpers are the only things here that reason
 * about distance between screen lines. */

/* The viewport top: a row and a sub-line within it.
 *
 * topRead() and topSet() are the only things that touch rowoff and
 * skip_sublines. 
 * Neither clamps the sub-line against the row's sub-line count.  A
 * width change moves wrap points under a stored skip, and the walkers
 * clamp it when they reach it, which costs nothing; asking the row for
 * its sub-line count here would be an O(row length) walk on every
 * read.  Clamp on read, not on resize. */
struct viewportTop {
	int row;
	int subline;
};

static inline struct viewportTop topRead(struct window *win,
					 struct buffer *buf) {
	struct viewportTop t;
	t.row = win->rowoff;
	if (t.row < 0)
		t.row = 0;
	if (t.row > buf->numrows - 1)
		t.row = buf->numrows - 1;
	t.subline = buf->word_wrap && win->skip_sublines > 0 ?
			    win->skip_sublines :
			    0;
	return t;
}

/* Move the top to (row, subline).  A row outside the buffer is clamped
 * and lands on that row's first screen line. */
static inline void topSet(struct window *win, struct buffer *buf, int row,
			  int subline) {
	int clamped = row;
	if (clamped < 0)
		clamped = 0;
	if (clamped > buf->numrows - 1)
		clamped = buf->numrows - 1;
	win->rowoff = clamped;
	win->skip_sublines =
		(clamped != row || !buf->word_wrap || subline < 0) ? 0 :
								     subline;
}

/* Screen lines from the viewport top to (row, subline): 0 if that is
 * the top line itself.  Returns -1 if the position is above the top or
 * more than `max` lines below it. */
static inline int linesFromTop(struct window *win, struct buffer *buf, int row,
			       int subline, int max) {
	struct viewportTop top = topRead(win, buf);
	if (row < top.row)
		return -1;
	if (!buf->word_wrap)
		return row - top.row <= max ? row - top.row : -1;
	if (row == top.row) {
		/* Sub-lines of one row are consecutive screen lines, so
		 * this is a subtraction. */
		int d = subline - top.subline;
		return d >= 0 && d <= max ? d : -1;
	}

	struct screenWalk w;
	screenWalkStart(&w, buf, E.screencols, top.row, top.subline);
	for (int n = 0; n <= max; n++) {
		if (w.row == row && w.subline == subline)
			return n;
		if (w.row > row)
			return -1;
		if (!screenWalkNext(&w))
			return -1;
	}
	return -1;
}

/* Screen lines from the viewport top to the end of the buffer, counting
 * the top line itself.  Stops at cap + 1, so a caller asking "does the
 * buffer end within `cap` lines" walks no further than the window. */
static inline int linesToEnd(struct window *win, struct buffer *buf, int cap) {
	struct viewportTop top = topRead(win, buf);
	if (!buf->word_wrap) {
		int n = buf->numrows - top.row;
		return n > cap ? cap + 1 : n;
	}

	struct screenWalk w;
	screenWalkStart(&w, buf, E.screencols, top.row, top.subline);
	int n = 1;
	while (n <= cap) {
		if (!screenWalkNext(&w))
			return n;
		n++;
	}
	return cap + 1;
}

/* How many screen lines the viewport may move, given a request of `n`.
 *
 * One rule for both modes, stated in screen lines: scrolling down stops
 * when the buffer's last screen line is within height - 2 lines of the
 * top, i.e. with two blank lines showing.
 *
 * The result may be negative, which is not an error: it means the top
 * already sits below the rule -- after a resize, or a deletion -- and
 * the distance back up is the same arithmetic.  It is clamped to n, so
 * a request is never exceeded in either direction.
 *
 * A window too short for the rule (height <= 2) gives a negative cap,
 * which no count can be under, so scrolling is bounded only by the end
 * of the buffer. */
static inline int scrollAllowance(struct window *win, struct buffer *buf,
				  int n) {
	/* Going up is bounded by the start of the buffer, which
	 * topSet() clamps -- not by the rule. */
	if (n < 0)
		return n;

	int cap = win->height - 2;
	int steps = linesToEnd(win, buf, cap + n) - cap;
	if (steps < 0)
		steps = 0;
	return steps > n ? n : steps;
}

/* Walk back `n` screen lines from (row, subline), reporting where it
 * lands.  Stops at the start of the buffer.  Sub-lines within the
 * starting row cost nothing; each earlier row costs its own wrap.
 *
 * That per-row cost is a whole-row walk, not a bounded one: entering a
 * row from below means starting at its LAST sub-line, and the only way
 * to number that is countScreenLines() over the entire row.  Under
 * wrap this is the dominant cost of scroll()'s below-window branch, of
 * page-up and of recenter() on long rows. */
static inline void linesBack(struct buffer *buf, int row, int subline, int n,
			     int *out_row, int *out_subline) {
	while (n > 0) {
		if (subline > 0) {
			subline--;
		} else {
			if (row <= 0)
				break;
			row--;
			subline = buf->word_wrap ?
					  countScreenLines(&buf->row[row],
							   E.screencols) -
						  1 :
					  0;
		}
		n--;
	}
	*out_row = row;
	*out_subline = subline;
}

/* Move the viewport top one screen line, down (dir > 0) or up.
 * Returns 0 if it could not move.  The non-wrap case is a row step
 * with no sub-line to carry, which is a branch here rather than a
 * guard at every caller. */
static inline int topAdvance(struct window *win, struct buffer *buf, int dir) {
	struct viewportTop top = topRead(win, buf);

	if (dir > 0) {
		if (buf->word_wrap) {
			struct screenWalk w;
			screenWalkStart(&w, buf, E.screencols, top.row,
					top.subline);
			if (!screenWalkNext(&w))
				return 0;
			topSet(win, buf, w.row, w.subline);
			return 1;
		}
		if (top.row >= buf->numrows - 1)
			return 0;
		topSet(win, buf, top.row + 1, 0);
		return 1;
	}

	if (top.row <= 0 && top.subline <= 0)
		return 0;
	int row, subline;
	linesBack(buf, top.row, top.subline, 1, &row, &subline);
	topSet(win, buf, row, subline);
	return 1;
}

/* Scroll the viewport by `n` screen lines.  Positive = down (content
 * moves up), negative = up (content moves down).  Handles both wrap
 * and non-wrap modes, managing rowoff and skip_sublines.
 *
 * Callers are responsible for adjusting the cursor afterwards. */
void scrollViewport(struct window *win, struct buffer *buf, int n) {
	if (n == 0)
		return;

	/* rowoff is not adjusted when an edit deletes rows, so on entry
	 * it may name a row that no longer exists.  refreshScreen clamps
	 * it, but that is a per-frame guarantee and a macro or a
	 * uarg-repeated command runs many operations between frames,
	 * while the word-wrap path below dereferences buf->row[rowoff]
	 * directly.  Clamp on entry rather than rely on refresh timing;
	 * topSet() is where the clamp lives. */
	topSet(win, buf, win->rowoff, win->skip_sublines);

	if (!buf->word_wrap) {
		/* A screen line is a row here, so applying the rule is
		 * the whole of it: no walking, and one expression for
		 * both directions.*/
		topSet(win, buf, win->rowoff + scrollAllowance(win, buf, n), 0);
		return;
	}

	/* Word-wrap mode: one screen line per step.  topAdvance carries
	 * from the sub-line into the row and back, so neither direction
	 * spells that out here. */
	if (n > 0) {
		/* The same rule the non-wrap branch applies; here
		 * it says how many steps to take rather than where to
		 * land, because a screen line is not a row. */
		int steps = scrollAllowance(win, buf, n);
		/* One walk for the whole descent: stepping through
		 * topAdvance would re-enter the row to find its sub-line
		 * on every step, which is quadratic within a row long
		 * enough to fill the window by itself. */
		struct viewportTop top = topRead(win, buf);
		struct screenWalk w;
		screenWalkStart(&w, buf, E.screencols, top.row, top.subline);
		int moved = 0;
		for (int i = 0; i < steps; i++) {
			if (!screenWalkNext(&w))
				break;
			moved++;
		}
		if (moved > 0)
			topSet(win, buf, w.row, w.subline);
	} else {
		for (int i = 0; i < -n; i++) {
			if (!topAdvance(win, buf, -1))
				break;
		}
	}
}

/* Where the cursor sits within its own row: which sub-line, and which
 * column of it.
 *
 * A cursor exactly at the right edge of a sub-line belongs on the next
 * one. */
static inline void cursorSubline(struct buffer *buf, int cursor_col,
				 int *sub_line, int *sub_col) {
	*sub_line = 0;
	*sub_col = cursor_col;
	if (!buf->word_wrap)
		return;

	cursorScreenLine(&buf->row[buf->cy], cursor_col, E.screencols, sub_line,
			 sub_col);
}

/* Does this row's own first screen line sit above the viewport top?
 * True for any row before the top row, and for the top row itself when
 * the window starts part-way down it. */
static inline int rowStartsAboveTop(struct window *win, struct buffer *buf,
				    int row) {
	struct viewportTop top = topRead(win, buf);
	return row < top.row || (row == top.row && top.subline > 0);
}

/* The last row with any screen line in the window: walk forward from
 * the top for the window's height and see where it stops. */
static inline int lastVisibleRow(struct window *win, struct buffer *buf) {
	struct viewportTop top = topRead(win, buf);
	if (!buf->word_wrap) {
		int last = top.row + win->height - 1;
		return last > buf->numrows - 1 ? buf->numrows - 1 : last;
	}

	struct screenWalk w;
	screenWalkStart(&w, buf, E.screencols, top.row, top.subline);
	for (int n = 1; n < win->height; n++) {
		if (!screenWalkNext(&w))
			break;
	}
	return w.row;
}

/* Ensure the cursor is within the visible viewport.  If it has fallen
 * outside, drag it to the nearest visible row. Does not touch the viewport. 
 */
void clampCursorToViewport(struct window *win, struct buffer *buf) {
	if (!buf->word_wrap) {
		if (buf->cy < win->rowoff)
			buf->cy = win->rowoff;
		else if (buf->cy >= win->rowoff + win->height)
			buf->cy = win->rowoff + win->height - 1;
	} else if (rowStartsAboveTop(win, buf, buf->cy)) {
		/* Above the window: down to the first row that starts
		 * within it. */
		while (buf->cy < buf->numrows - 1) {
			buf->cy++;
			if (!rowStartsAboveTop(win, buf, buf->cy))
				break;
		}
		buf->cx = 0;
	} else {
		/* Below the window: up to the last row it shows.  Found
		 * by one bounded walk rather than a test per row. */
		int last = lastVisibleRow(win, buf);
		if (buf->cy > last)
			buf->cy = last;
	}

	if (buf->cy < 0)
		buf->cy = 0;
	if (buf->cy >= buf->numrows)
		buf->cy = buf->numrows - 1;
	if (buf->cx > buf->row[buf->cy].size)
		buf->cx = buf->row[buf->cy].size;
}

/* Render a line with highlighting support.
 *
 * start_col / end_col: the display-column range to render.
 * start_byte: byte offset in row->chars corresponding to start_col,
 *             or -1 to scan from the beginning.  The word-wrap caller
 *             already knows the byte offset; passing it in avoids an
 *             O(line-length) skip loop for every wrapped sub-line.
 *
 * Returns the display column rendering stopped at -- end_col if the row
 * ran past the right edge, otherwise the row's width.  The caller pads
 * from it rather than asking the row how wide it is. */
static int renderLineWithHighlighting(erow *row, struct abuf *ab, int start_col,
				      int end_col,
				      const struct rowHighlight *hl,
				      int start_byte) {
	int render_x = 0;
	int char_idx = 0;
	int current_highlight = 0;

	/* Skip to start column.  If the caller provided a byte hint we
	 * can jump straight there; otherwise scan from byte 0. */
	if (start_byte >= 0 && start_byte <= row->size) {
		char_idx = start_byte;
		render_x = start_col;
	} else {
		while (char_idx < row->size && render_x < start_col) {
			if (row->chars[char_idx] < 0x80 &&
			    !ISCTRL(row->chars[char_idx])) {
				render_x += 1;
				char_idx++;
			} else {
				render_x = nextScreenX(row->chars, &char_idx,
						       render_x);
				char_idx++;
			}
		}
	}

	/* A tab or double-width character that straddles start_col is
	 * consumed whole by the skip loop, leaving render_x past
	 * start_col.  Emit that character's still-visible columns as
	 * spaces; without this they are dropped, the rest of the line
	 * shifts left, and the cursor no longer sits on its character.
	 * No-op when the caller passed start_byte (word-wrap), since a
	 * sub-line always begins on a character boundary. */
	if (render_x > start_col) {
		int pad_hl = 0;
		for (int col = start_col; col < render_x && col < end_col;
		     col++) {
			updateHighlight(ab, &pad_hl,
					isHighlighted(hl, col) ? 1 : 0);
			abAppend(ab, " ", 1);
		}
		updateHighlight(ab, &pad_hl, 0);
	}

	/* Render visible portion.  What each byte class EMITS stays
	 * per-branch (spaces for a tab, ^X for a control, raw bytes
	 * otherwise), but how far it ADVANCES comes from charAdvance —
	 * the same rule wordWrapBreak used when it decided where this
	 * sub-line ends (#117 R1).  If the two priced a byte
	 * differently, text would shift within the sub-line, the fill
	 * loop would mis-pad, and the cursor would leave its
	 * character; sharing the rule removes the possibility rather
	 * than the instances. */
	while (char_idx < row->size && render_x < end_col) {
		uint8_t c = row->chars[char_idx];

		updateHighlight(ab, &current_highlight,
				isHighlighted(hl, render_x) ? 1 : 0);

		int nb;
		int width = charAdvance(row->chars, char_idx, render_x, &nb);

		if (c == '\t') {
			/* width is the distance to the next tab stop. */
			int next_tab_stop = render_x + width;
			while (render_x < next_tab_stop && render_x < end_col) {
				if (render_x >= start_col) {
					abAppend(ab, " ", 1);
				}
				render_x++;
			}
		} else if (ISCTRL(c)) {
			if (render_x >= start_col) {
				abAppend(ab, "^", 1);
				if (c == 0x7f) {
					abAppend(ab, "?", 1);
				} else {
					char sym = c | 0x40;
					abAppend(ab, &sym, 1);
				}
			}
			render_x += width; /* == 2 */
		} else {
			if (render_x >= start_col) {
				abAppend(ab, (char *)&row->chars[char_idx], nb);
			}
			render_x += width;
		}

		char_idx += nb;
	}

	updateHighlight(ab, &current_highlight, 0);
	return render_x;
}

/* Display functions */

/* The cursor's position within its window, in screen cells relative to
 * the window's top-left corner.  Derived wholly from the window and its
 * buffer, and read only for the focused window when placing the cursor
 * at the end of a frame, so it is returned rather than stored.
 *
 * cursor_col is the display column of (cx, cy), which scroll() has
 * already computed for the same position this frame; -1 means compute
 * it here.  Recomputing it is O(cx), so on a long line the duplicate
 * cost the frame as much as the walk itself. */
void screenCursorPos(struct window *win, const struct cursorHint *hint,
		     int *scx_out, int *scy_out) {
	struct buffer *buf = win->buf;
	if (hint) {
		/* scrollFocused() placed this cursor earlier in the frame
		 * and reported where it landed; recomputing it here walked
		 * the row and the viewport a second time (#116). */
		*scx_out = hint->scx;
		*scy_out = hint->scy;
		return;
	}
	int cursor_col = -1;
	erow *row = &buf->row[buf->cy]; /* cy < numrows (#105) */
	int total_width = cursor_col >= 0 ? cursor_col :
					    charsToDisplayColumn(row, buf->cx);
	int sub_line, sub_col;
	cursorSubline(buf, total_width, &sub_line, &sub_col);

	int scx = buf->word_wrap ? sub_col : total_width - win->coloff;
	if (scx < 0)
		scx = 0;

	/* Screen lines from the top of the window to the cursor's own
	 * screen line.  Off screen has no distance to report, and the
	 * frame has to put the cursor somewhere, so it goes on the
	 * nearest edge */
	int scy = linesFromTop(win, buf, buf->cy, sub_line, win->height - 1);
	if (scy < 0) {
		struct viewportTop top = topRead(win, buf);
		int above = buf->cy < top.row ||
			    (buf->cy == top.row && sub_line < top.subline);
		scy = above ? 0 : win->height - 1;
	}
	/* A window of no height has no line to report, and height - 1
	 * above is then negative.  The frame has to put the cursor
	 * somewhere; row 0 of the window is the only defensible
	 * answer, and a negative one would address the wrong row. */
	if (scy < 0)
		scy = 0;

	*scx_out = scx;
	*scy_out = scy;
}

/* The vertical half of viewport placement: move `win`'s top so the
 * cursor's screen line is inside the window, and report where the
 * cursor landed.
 *
 * `cursor_col` is the display column of (cx, cy) and is read only
 * under word wrap.  The screen row and the column within the sub-line
 * fall out of the placement decision, so they are reported rather
 * than recomputed by a caller that wants them (#116). */
static void scrollVertical(struct window *win, struct buffer *buf,
			   int cursor_col, int *out_scy, int *out_sub_col) {
	int scy = 0, sub_col = cursor_col;

	if (buf->word_wrap) {
		int sub_line;
		cursorSubline(buf, cursor_col, &sub_line, &sub_col);

		struct viewportTop top = topRead(win, buf);
		int above = buf->cy < top.row ||
			    (buf->cy == top.row && sub_line < top.subline);

		if (above) {
			/* Above the window: the cursor's own screen line
			 * becomes the top one. */
			topSet(win, buf, buf->cy, sub_line);
			scy = 0;
		} else {
			int n = linesFromTop(win, buf, buf->cy, sub_line,
					     win->height - 1);
			if (n < 0) {
				/* Below it: put the cursor on the window's
				 * last screen line by walking back
				 * height - 1 from it. */
				int row, subline;
				linesBack(buf, buf->cy, sub_line,
					  win->height - 1, &row, &subline);
				topSet(win, buf, row, subline);
				scy = win->height - 1;
			} else {
				/* Visible: the top stays. */
				scy = n;
			}
		}
	} else {
		/* topSet() zeroes the skip: there are no sub-lines here. */
		if (buf->cy < win->rowoff)
			topSet(win, buf, buf->cy, 0);
		else if (buf->cy >= win->rowoff + win->height)
			topSet(win, buf, buf->cy - win->height + 1, 0);
		else
			topSet(win, buf, win->rowoff, 0);
		scy = buf->cy - win->rowoff;
	}

	if (out_scy)
		*out_scy = scy;
	if (out_sub_col)
		*out_sub_col = sub_col;
}

/* Returns the display column of the focused cursor, which it computes
 * to place the viewport.  screenCursorPos() and the status bar want the
 * same number for the same position, and each walk is O(cx). */
int scrollFocused(struct cursorHint *hint) {
	struct window *win = E.windows[windowFocusedIdx()];
	struct buffer *buf = win->buf;

	if (buf->cy > buf->numrows - 1) {
		buf->cy = buf->numrows - 1;
		buf->cx = buf->row[buf->cy].size;
	} else if (buf->cx > buf->row[buf->cy].size) {
		buf->cx = buf->row[buf->cy].size;
	}

	int cursor_col = charsToDisplayColumn(&buf->row[buf->cy], buf->cx);
	int scy, sub_col;
	scrollVertical(win, buf, cursor_col, &scy, &sub_col);

	if (!buf->word_wrap) {
		if (cursor_col < win->coloff) {
			win->coloff = cursor_col;
		} else if (cursor_col >= win->coloff + E.screencols) {
			win->coloff = cursor_col - E.screencols + 1;
		}
	} else {
		win->coloff = 0;
	}

	if (hint) {
		hint->col = cursor_col;
		hint->scx = buf->word_wrap ? sub_col : cursor_col - win->coloff;
		if (hint->scx < 0)
			hint->scx = 0;
		hint->scy = scy;
		if (hint->scy > win->height - 1)
			hint->scy = win->height - 1;
		if (hint->scy < 0)
			hint->scy = 0;
	}
	return cursor_col;
}

int scroll(void) {
	return scrollFocused(NULL);
}

/* Move `win`'s viewport so that `buf->cy` is visible, exactly as the
 * vertical half of scroll() does for the focused window. Unlike
 * scroll(), this does not require `win` to be the focused window --
 * it exists for windows that are never focused, such as the
 * completions popup, whose selection cursor (buf->cy) moves in
 * response to M-p/M-n while focus stays on the prompt's minibuffer.
 * scroll() runs for the focused window only (see adjust.c's note on
 * the same gap for a different cause), so nothing else keeps such a
 * window's viewport following its own cursor.
 *
 * Horizontal scrolling (coloff) and cx clamping are deliberately not
 * handled here: they are concerns of the window actually being typed
 * into, not of a passive popup. */
void scrollToShowCursor(struct window *win, struct buffer *buf) {
	if (buf->cy < 0)
		buf->cy = 0;
	if (buf->cy > buf->numrows - 1)
		buf->cy = buf->numrows - 1;

	int cursor_col =
		buf->word_wrap ?
			charsToDisplayColumn(&buf->row[buf->cy], buf->cx) :
			0;
	scrollVertical(win, buf, cursor_col, NULL, NULL);
}

void drawRows(struct window *win, struct abuf *ab, int screenrows,
	      int screencols) {
	struct buffer *buf = win->buf;
	int y;
	int filerow = win->rowoff;
	int skip = win->skip_sublines; /* sub-lines to skip on first row */
	int truncated = 0; /* a row was cut off by the window's bottom */

	for (y = 0; y < screenrows; y++) {
		int filled =
			0; /* set when word-wrap fill loop padded this line */
		if (filerow >= buf->numrows) {
			abAppend(ab, " ", 1);
		} else {
			erow *row = &buf->row[filerow];
			if (!buf->word_wrap) {
				// Truncated mode with visual marking
				int end_col = win->coloff + screencols;
				struct rowHighlight hl;
				computeRowHighlightBounds(buf, filerow, end_col,
							  &hl);
				/* Pad remainder of screen line so \x1b[K
				 * is not needed (it would erase the last
				 * column due to pending-wrap state).  The
				 * renderer has just emitted this content and
				 * knows the column it stopped at; asking the
				 * row for its full width instead was an
				 * O(row length) walk per frame (§C2). */
				int emitted = renderLineWithHighlighting(
					row, ab, win->coloff, end_col, &hl, -1);
				int rx = emitted - win->coloff;
				if (rx < 0)
					rx = 0;
				while (rx < screencols) {
					abAppend(ab, " ", 1);
					rx++;
				}
				filled = 1;
				filerow++;
			} else {
				/* Word-wrap mode: break lines at word
				 * boundaries when possible. */
				int line_start_col = 0;
				int line_start_byte = 0;
				int sub_line_idx = 0;

				/* Wrapped columns accumulate across
				 * sub-lines, so the row's own columns run
				 * past the window width; INT_MAX is the
				 * bound here. */
				struct rowHighlight hl;
				computeRowHighlightBounds(buf, filerow, INT_MAX,
							  &hl);

				while (line_start_byte < row->size &&
				       y < screenrows) {
					int break_col, break_byte;
					int more = wordWrapBreak(
						row, screencols, line_start_col,
						line_start_byte, &break_col,
						&break_byte);

					/* Skip sub-lines that are above the
					 * visible area (only for the first
					 * rendered row, i.e. rowoff) */
					if (sub_line_idx < skip) {
						if (more) {
							sub_line_idx++;
							line_start_col =
								break_col;
							line_start_byte =
								break_byte;
							continue;
						}
						/* The skip names a sub-line
						 * this row does not have: the
						 * row shrank, or the terminal
						 * widened, under a stored top.
						 * Nothing clamps it on write
						 * so clamp it here and draw
						 * the row's last sub-line.
						 * Breaking out instead left
						 * the window's top line blank.
						 */
						skip = sub_line_idx;
					}

					/* --- Render the span --- */
					renderLineWithHighlighting(
						row, ab, line_start_col,
						break_col, &hl,
						line_start_byte);

					/* --- Fill trailing space with
					 *     correct highlighting --- */
					int fill_col = break_col;
					int fill_hl = 0;
					while (fill_col - line_start_col <
					       screencols) {
						updateHighlight(
							ab, &fill_hl,
							isHighlighted(
								&hl, fill_col) ?
								1 :
								0);
						abAppend(ab, " ", 1);
						fill_col++;
					}
					updateHighlight(ab, &fill_hl, 0);

					filled = 1;

					if (!more)
						break;

					/* Advance to next screen line if
					 * room remains; otherwise the
					 * window is full. */
					if (y < screenrows - 1) {
						abAppend(ab, "\r\n", 2);
						y++;
					} else {
						truncated = 1;
						break;
					}
					line_start_col = break_col;
					line_start_byte = break_byte;
					sub_line_idx++;
				}

				filerow++;
				skip = 0; /* Only skip on the first row */
			}
		}
		if (!filled)
			abAppend(ab, "\x1b[K", 3);
		if (y < screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}

	/* Is the last buffer line visible?  The loop above just answered
	 * that by running out of rows (or not) while filling the window;
	 * asking again afterwards walked the same span a second time
	 * (#116).
	 *
	 * One expression for both modes.  The two formulas this replaced
	 * disagreed by one line: without wrap the flag demanded a blank
	 * line past the last row, so the bar reported a percentage where
	 * wrap reported `Bot` for the identical picture.  §5.1.2 says
	 * `Bot` when the end of the buffer is visible, which is this. */
	buf->end = (filerow >= buf->numrows && !truncated);
}

/* Truncate a UTF-8 string to fit within `max_cols` display columns,
 * writing the result into `out` (which must have room for the result
 * plus a NUL).
 *
 * Returns the number of display columns written.  The output may be
 * shorter than max_cols if the next character would straddle the
 * boundary (e.g. a 2-column CJK glyph with only 1 column left).
 *
 * Column accounting is utf8ColsToBytes, THE shared walk (#117 R1);
 * only the output-capacity clamp is local. */
static int truncateToCols(char *out, size_t out_cap, const char *in,
			  int max_cols) {
	const uint8_t *p = (const uint8_t *)in;
	int len = (int)strlen(in);

	int cols = 0;
	int nbytes = utf8ColsToBytes(p, 0, len, max_cols, &cols);

	/* Clamp to the output buffer, dropping whole characters. */
	while (nbytes > 0 && (size_t)nbytes + 1 > out_cap) {
		int back = nbytes - 1;
		while (back > 0 && utf8_isCont(p[back]))
			back--;
		int nb;
		cols -= charAdvance(p, back, 0, &nb);
		nbytes = back;
	}

	memcpy(out, in, (size_t)nbytes);
	out[nbytes] = '\0';
	return cols;
}

/* Status bar block renderers.  Each writes its content into a
 * caller-supplied buffer and returns the byte length written.
 * The BLOCK constant (15 columns) governs the fixed-width mid
 * and right blocks; the left block gets the remainder. */

static const int STATUS_BLOCK = 15;

/* Left block: display name + flags (e.g. "file.c **%").
 * name_width is the column budget for the name portion. */
static int statusLeft(const struct buffer *bufr, char *out, int cap,
		      int name_width) {
	const char *dname =
		bufr->display_name ?
			bufr->display_name :
			(bufr->filename ? bufr->filename : "*scratch*");

	/* Two-character flag field, as Emacs writes it:
	 *
	 *   --   clean, writable
	 *   **   modified, writable
	 *   %%   clean, read-only
	 *   %*   modified, read-only
	 *
	 * Both properties share the same two columns: the left carries
	 * read-only, the right carries modified, and each doubles the
	 * other's character when it has nothing of its own to say. */
	char flags[4];
	if (bufr->read_only) {
		flags[0] = '%';
		flags[1] = bufr->dirty ? '*' : '%';
	} else {
		flags[0] = bufr->dirty ? '*' : '-';
		flags[1] = flags[0];
	}
	flags[2] = '\0';

	/* Left-truncate name with "..." to fit name_width, which is a
	 * budget in COLUMNS.  Drop leading whole characters until what
	 * remains fits the budget "..." leaves: indexing by bytes
	 * instead would land inside a multi-byte sequence and emit
	 * invalid UTF-8 (B12).  Widths come from charAdvance, the one
	 * rule (#117 R1) — this walk and leftTruncate in buffer.c both
	 * truncate display_name, so they must price it the same way. */
	const char *show_name = dname;
	char trunc[256];
	const uint8_t *u = (const uint8_t *)dname;
	int dlen = (int)strlen(dname);
	if (utf8WidthN(u, dlen) > name_width) {
		int tail_cols = name_width - 3;
		if (tail_cols < 1)
			tail_cols = 1;
		snprintf(trunc, sizeof(trunc), "...%s",
			 dname + utf8DropToFit(u, dlen, tail_cols));
		show_name = trunc;
	}

	/* snprintf returns the length it WOULD have written.  The caller
	 * hands this straight to abAppend against a fixed-size stack
	 * buffer, so an over-long name made it read past the end.  Clamp
	 * to what actually fits. */
	int n = snprintf(out, cap, "%s %s", show_name, flags);
	if (n < 0)
		return 0;
	if (n >= cap)
		n = cap - 1;
	return n;
}

/* Middle block: line:col + position indicator, padded to STATUS_BLOCK.
 * Returns byte count written (always STATUS_BLOCK on success). */
static int statusMid(const struct window *win, char *out, char fc,
		     int cursor_col) {
	struct buffer *bufr = win->buf;
	const char *sep = win->focused ? "  " : "--";

	/* The column is a DISPLAY column, not a byte offset.  
	 *
	 * Line stays one-based and column zero-based, matching Emacs
	 * including its asymmetry.
	 *
	 * Cost: the walk is O(the cursor's distance from its line start),
	 * which on a long line is the frame's whole budget.  The focused
	 * window does not pay it: scroll() computed this exact column for
	 * this exact position earlier in the frame and passes it in as
	 * cursor_col.  A non-focused window reports its own saved cursor,
	 * which nothing else in the frame asks about, so it computes. */
	int ry = win->focused ? bufr->cy + 1 : win->cy + 1;
	int cur_y = win->focused ? bufr->cy : win->cy;
	int cur_x = win->focused ? bufr->cx : win->cx;
	int rx = cur_x;
	if (cursor_col >= 0)
		rx = cursor_col;
	else if (cur_y >= 0 && cur_y < bufr->numrows)
		rx = charsToDisplayColumn(&bufr->row[cur_y], cur_x);
	char linecol[24];
	int linecol_len =
		snprintf(linecol, sizeof(linecol), "%s%d:%d", sep, ry, rx);
	if (linecol_len < 0)
		linecol_len = 0;
	char pos[8];
	if (bufferIsEmpty(bufr))
		memcpy(pos, "Emp", 4);
	else if (bufr->end && win->rowoff == 0)
		memcpy(pos, "All", 4);
	else if (bufr->end)
		memcpy(pos, "Bot", 4);
	else if (win->rowoff == 0)
		memcpy(pos, "Top", 4);
	else
		/* 100LL, so the multiplication is done at 64 bits.*/
		snprintf(pos, sizeof(pos), "%2d%%",
			 (int)((win->rowoff * 100LL) / bufr->numrows));

	int lc = linecol_len < STATUS_BLOCK ? linecol_len : STATUS_BLOCK;
	int pos_len = strlen(pos);
	int gap = STATUS_BLOCK - lc - pos_len;
	int oi = 0;

	memcpy(out, linecol, lc);
	oi = lc;
	if (gap > 0) {
		memset(out + oi, fc, gap);
		oi += gap;
	}
	if (oi + pos_len <= STATUS_BLOCK) {
		memcpy(out + oi, pos, pos_len);
		oi += pos_len;
	}
	while (oi < STATUS_BLOCK)
		out[oi++] = fc;
	return oi;
}

/* Right block: warning or mode indicator, right-aligned, padded to
 * STATUS_BLOCK.  Returns values via *out_bytes and *out_cols because
 * multi-byte UTF-8 warnings make byte length diverge from column
 * count.
 *
 * Precedence (highest first):
 *   1. FILE MODIFIED
 *   2. NNNN LOCK / LOCKED
 *   3. (Macro) / (Wrap) / (Macro Wrap)
 */
static void statusRight(const struct window *win, char *out, int *out_bytes,
			int *out_cols, char fc) {
	struct buffer *bufr = win->buf;
	const char *sep = win->focused ? "  " : "--";
	const int CONTENT = STATUS_BLOCK - 2;

	/* Determine warning text (if any) */
	char warn_buf[64];
	const char *warn = NULL;
	if (bufr->external_mod) {
		snprintf(warn_buf, sizeof(warn_buf), "%s", "FILE MODIFIED");
		warn = warn_buf;
	} else if (bufr->lock_blocked_pid > 0) {
		snprintf(warn_buf, sizeof(warn_buf), "%d LOCK",
			 bufr->lock_blocked_pid);
		warn = warn_buf;
	} else if (bufr->lock_blocked_pid != 0) {
		/* Holder unknown: F_GETLK named no PID.  Sentinels must
		 * never reach the user as "-1 LOCK". */
		snprintf(warn_buf, sizeof(warn_buf), "%s", "LOCKED");
		warn = warn_buf;
	}

	if (warn) {
		char tmp[64];
		int content_cols =
			truncateToCols(tmp, sizeof(tmp), warn, CONTENT);
		int tmp_bytes = (int)strlen(tmp);
		int left_pad = CONTENT - content_cols;
		if (left_pad < 0)
			left_pad = 0;
		int sep_len = (int)strlen(sep);
		if (sep_len + left_pad + tmp_bytes + 1 > 64) {
			tmp_bytes = 64 - sep_len - left_pad - 1;
			if (tmp_bytes < 0)
				tmp_bytes = 0;
		}
		memcpy(out, sep, sep_len);
		memset(out + sep_len, fc, left_pad);
		memcpy(out + sep_len + left_pad, tmp, tmp_bytes);
		*out_bytes = sep_len + left_pad + tmp_bytes;
		*out_cols = STATUS_BLOCK;
		return;
	}

	/* Mode indicators (no warning active) */
	const char *paren = NULL;
	if (E.recording && bufr->word_wrap)
		paren = "(Macro Wrap)";
	else if (E.recording)
		paren = "(Macro)";
	else if (bufr->word_wrap)
		paren = "(Wrap)";

	if (paren) {
		int clen = snprintf(out, 64, "%s%s", sep, paren);
		int pad = STATUS_BLOCK - clen;
		if (pad > 0) {
			memmove(out + pad, out, clen);
			memset(out, fc, pad);
		}
		*out_bytes = STATUS_BLOCK;
		*out_cols = STATUS_BLOCK;
		return;
	}

	/* Empty: fill with fc */
	memset(out, fc, STATUS_BLOCK);
	*out_bytes = STATUS_BLOCK;
	*out_cols = STATUS_BLOCK;
}

/* Join the three status blocks into the abuf with fill padding. */
static void joinStatusBlocks(struct abuf *ab, const char *left, int left_len,
			     const char *mid, int mid_len, const char *rhs,
			     int rhs_bytes, int rhs_cols, int total, char fc) {
	abAppend(ab, left, left_len);

	int gap = total - left_len - mid_len - rhs_cols;
	while (gap-- > 0)
		abAppend(ab, &fc, 1);

	if (mid_len > 0)
		abAppend(ab, mid, mid_len);
	if (rhs_bytes > 0)
		abAppend(ab, rhs, rhs_bytes);
}

/* cursor_col is the display column of the cursor this bar reports, or
 * -1 to compute it.  Only the focused window's caller knows it. */
void drawStatusBar(struct window *win, struct abuf *ab, int line,
		   int cursor_col) {
	char buf[32];
	snprintf(buf, sizeof(buf), CSI "%d;%dH", line, 1);
	abAppend(ab, buf, strlen(buf));

	struct buffer *bufr = win->buf;
	abAppend(ab, "\x1b[7m", 4);

	int total = E.screencols;
	int focused = win->focused;
	char fc = focused ? ' ' : '-';

	/* Compute name width budget */
	const char *dname =
		bufr->display_name ?
			bufr->display_name :
			(bufr->filename ? bufr->filename : "*scratch*");
	int dlen = strlen(dname);
	const char *bname = strrchr(dname, '/');
	bname = bname ? bname + 1 : dname;
	int bname_len = strlen(bname);

	int has_path = (dlen > bname_len);
	int min_name = has_path ? bname_len + 3 : bname_len;
	if (bufr->min_name_len > min_name)
		min_name = bufr->min_name_len;
	if (min_name > dlen)
		min_name = dlen;

	int flags_len = 2; /* "XY": read-only column, modified column */
	int name_need = 1 + flags_len + min_name; /* space + flags + name */
	int remain = total - name_need;

	int have_mid = (remain >= STATUS_BLOCK);
	if (have_mid)
		remain -= STATUS_BLOCK;
	int have_rhs = (remain >= STATUS_BLOCK);
	if (have_rhs)
		remain -= STATUS_BLOCK;

	int name_width = min_name + (remain > 0 ? remain : 0);
	int max_name = total - 1 - flags_len;
	if (max_name < 1)
		max_name = 1;
	if (name_width > max_name)
		name_width = max_name;

	/* Render blocks */
	char left[512];
	int left_len = statusLeft(bufr, left, sizeof(left), name_width);

	char mid[16];
	int mid_len = 0;
	if (have_mid)
		mid_len = statusMid(win, mid, fc, cursor_col);

	char rhs[64];
	int rhs_bytes = 0, rhs_cols = 0;
	if (have_rhs)
		statusRight(win, rhs, &rhs_bytes, &rhs_cols, fc);

	joinStatusBlocks(ab, left, left_len, mid, mid_len, rhs, rhs_bytes,
			 rhs_cols, total, fc);

	abAppend(ab, "\x1b[m" CRLF, 5);
}
/* THE minibuffer layout model (#117 R3).
 *
 * The minibuffer is a wrapped text surface, and three places used to
 * model its geometry independently: refreshScreen sized it by
 * division, drawMinibuffer filled it character by character, and
 * cursorBottomLine divided again while ignoring both the prefix and
 * character width.  The division models under-counted by up to one
 * column per line, accumulating, so any odd terminal width plus a
 * message containing CJK or emoji silently lost its last line
 * (DEF-2): a CJK filename in a save message, C-x = output, the
 * palette's Unicode-name line.
 *
 * All three now run this.  It breaks msg into at most max_lines
 * screen lines, splitting only at character boundaries, with a wide
 * character that straddles the boundary pushed to the next line (that
 * line comes up one column short — the behaviour drawMinibuffer
 * always had and the sizing never knew about).  Line 0 shares its
 * width with prefix_display.
 *
 * Widths come from charAdvance via utf8ColsToBytes (#117 R1).
 *
 * Deliberately NOT routed through wordWrapBreak: this wraps a C
 * string, not an erow, and wants no word-boundary breaks. */
int minibufLayout(const char *msg, int prefix_cols, int screencols,
		  struct minibufLine *out, int max_lines) {
	int msglen = msg ? (int)strlen(msg) : 0;
	int offset = 0;
	int n = 0;

	do {
		int avail = screencols - (n == 0 ? prefix_cols : 0);
		if (avail < 1)
			avail = 1;

		int used = 0;
		int next = utf8ColsToBytes((const uint8_t *)msg, offset,
					   msglen - offset, avail, &used);

		/* Guarantee progress: if not even one character fit
		 * (a wide character on a 1-column remainder, or
		 * degenerate widths), force one whole character
		 * through so the message cannot stall across lines. */
		if (next == offset && offset < msglen) {
			int nb;
			used = charAdvance((const uint8_t *)msg, offset, 0,
					   &nb);
			next = offset + nb;
		}

		out[n].start = offset;
		out[n].end = next;
		out[n].cols = used;
		n++;
		offset = next;
	} while (offset < msglen && n < max_lines);

	return n;
}

/* Screen lines the current status message needs, capped at
 * MINIBUF_MAX_LINES (§5.5).
 *
 * A named function rather than inline arithmetic in refreshScreen so
 * that a test can assert it against what drawMinibuffer actually
 * emits — the two disagreeing is DEF-2, and inline code left the
 * disagreement unobservable. */
int minibufHeightNeeded(void) {
	if (!(E.statusmsg_show && E.statusmsg[0] && E.screencols > 0))
		return 1;

	struct minibufLine lines[MINIBUF_MAX_LINES];
	int prefix_cols = stringWidth((const uint8_t *)E.prefix_display);
	return minibufLayout(E.statusmsg, prefix_cols, E.screencols, lines,
			     MINIBUF_MAX_LINES);
}

void drawMinibuffer(struct abuf *ab) {
	/* Determine the message to display */
	const char *msg = E.statusmsg;
	int msglen = strlen(msg);
	int valid = msglen && E.statusmsg_show;

	/* Prefix takes space on the first line; its screen footprint
	 * is its display width, not its byte length */
	int prefix_len = strlen(E.prefix_display);
	int prefix_cols = stringWidth((const uint8_t *)E.prefix_display);

	struct minibufLine lines[MINIBUF_MAX_LINES];
	int nlines = 0;
	if (valid)
		nlines = minibufLayout(msg, prefix_cols, E.screencols, lines,
				       MINIBUF_MAX_LINES);

	/* Draw each minibuffer line */
	for (int line = 0; line < minibuffer_height; line++) {
		abAppend(ab, "\x1b[K", 3); /* clear line */

		if (!valid) {
			if (line < minibuffer_height - 1)
				abAppend(ab, "\r\n", 2);
			continue;
		}

		if (line == 0 && prefix_len > 0)
			abAppend(ab, E.prefix_display, prefix_len);

		if (line < nlines) {
			int start = lines[line].start;
			int len = lines[line].end - start;
			if (len > 0)
				abAppend(ab, msg + start, len);
			abAppend(ab, "\x1b[0m", 4);
		}

		if (line < minibuffer_height - 1)
			abAppend(ab, "\r\n", 2);
	}
}

void refreshScreen(void) {
	/* Check for external modification of the focused buffer's file */
	checkFileModified();

	struct abuf *ab = &E.render_buf;
	ab->len = 0; /* Reset for this frame; keep the allocation */
	abAppend(ab, "\x1b[?7l", 5);  // Disable auto-wrap
	abAppend(ab, "\x1b[?25l", 6); // Hide cursor
	abAppend(ab, "\x1b[H", 3);    // Move cursor to top-left corner

	/* Mandatory bounds clamp for all windows: every window's
	 * rowoff must name a row that exists.*/
	for (int i = 0; i < E.nwindows; i++) {
		struct window *w = E.windows[i];
		struct buffer *b = w->buf;
		topSet(w, b, w->rowoff, w->skip_sublines);
	}

	int focusedIdx = windowFocusedIdx();

	/* Auto-size the minibuffer to fit the current status message,
	 * by running the same layout drawMinibuffer will (#117 R3).
	 * The previous division — 1 + ceil((msg_cols - first_line) /
	 * screencols) — assumed every line is filled to exactly
	 * screencols columns, which is false whenever a wide character
	 * straddles the boundary and is pushed to the next line.  The
	 * error was up to one column per line and accumulated, so the
	 * message's last line was silently dropped (DEF-2). */
	int needed_mb = minibufHeightNeeded();
	if (needed_mb != minibuffer_height) {
		minibuffer_height = needed_mb;
		for (int i = 0; i < E.nwindows; i++)
			E.windows[i]->height = 0;
	}

	int cumulative_height = 0;
	int total_height = E.screenrows - minibuffer_height -
			   (statusbar_height * E.nwindows);

	/* skip if heights already set */
	int heights_set = 1;
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->height <= 0) {
			heights_set = 0;
			break;
		}
	}

	if (!heights_set) {
		int window_height = total_height / E.nwindows;
		int remaining_height = total_height % E.nwindows;

		for (int i = 0; i < E.nwindows; i++) {
			struct window *win = E.windows[i];
			win->height = window_height;
			if (i == E.nwindows - 1)
				win->height += remaining_height;
		}
	}

	/* The focused cursor's display column, computed once by scroll()
	 * and reused by the status bar and the cursor placement below. */
	int cursor_col = -1;
	struct cursorHint hint;
	int have_hint = 0;

	for (int i = 0; i < E.nwindows; i++) {
		struct window *win = E.windows[i];

		if (win->focused) {
			cursor_col = scrollFocused(&hint);
			have_hint = 1;
		}
		drawRows(win, ab, win->height, E.screencols);
		cumulative_height += win->height + statusbar_height;
		drawStatusBar(win, ab, cumulative_height,
			      win->focused ? cursor_col : -1);
	}

	drawMinibuffer(ab);

	// Clear any remaining lines below content
	abAppend(ab, "\x1b[J", 3);

	// Position the cursor for the focused window
	struct window *focusedWin = E.windows[focusedIdx];
	char buf[32];

	int scx, scy;
	screenCursorPos(focusedWin, have_hint ? &hint : NULL, &scx, &scy);

	int cursor_y = scy + 1; // 1-based index
	for (int i = 0; i < focusedIdx; i++) {
		cursor_y += E.windows[i]->height + statusbar_height;
	}

	// Ensure cursor doesn't go beyond the window's bottom
	if (cursor_y > cumulative_height) {
		cursor_y = cumulative_height - statusbar_height;
	}

	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cursor_y, scx + 1);
	abAppend(ab, buf, strlen(buf));
	abAppend(ab, "\x1b[?7h", 5);  // Enable auto-wrap
	abAppend(ab, "\x1b[?25h", 6); // Show cursor

	/* Looped: a signal landing mid-write returns a short count and
	 * would drop the frame's tail (CSI ?7h, CSI ?25h), leaving
	 * auto-wrap off and the cursor hidden until the next full frame.
	 * Reachable at ordinary terminal sizes -- see writeAll(). */
	IGNORE_RETURN(writeAll(STDOUT_FILENO, ab->b, ab->len));
}

void cursorBottomLine(int curs) {
	char cbuf[32];
	/* Calculate actual minibuffer row position */
	int minibuf_row = 0;
	for (int i = 0; i < E.nwindows; i++) {
		minibuf_row += E.windows[i]->height + statusbar_height;
	}
	minibuf_row++; /* minibuffer starts after all windows/status bars */

	/* Which screen line the cursor falls on, and where within it.
	 *
	 * `curs` is a 1-based display column into the message stream:
	 * callers add the width of the prompt text (which lives inside
	 * E.statusmsg) to the columns of input before point.  Walking
	 * the same layout drawMinibuffer used is the only way to land
	 * on the character the user is editing — the previous code
	 * divided by screencols, ignoring both prefix_display and the
	 * short-by-one lines a straddling wide character produces, so
	 * the cursor drifted left of its character on a wrapped prompt
	 * (#117 R3). */
	if (minibuffer_height > 1 && E.screencols > 0) {
		struct minibufLine lines[MINIBUF_MAX_LINES];
		int prefix_cols =
			stringWidth((const uint8_t *)E.prefix_display);
		int n = minibufLayout(E.statusmsg, prefix_cols, E.screencols,
				      lines, MINIBUF_MAX_LINES);

		int target = curs - 1; /* columns into the message */
		if (target < 0)
			target = 0;

		int acc = 0;
		int li = 0;
		while (li < n - 1 && target > acc + lines[li].cols) {
			acc += lines[li].cols;
			li++;
		}
		if (li >= minibuffer_height)
			li = minibuffer_height - 1;

		minibuf_row += li;
		curs = (li == 0 ? prefix_cols : 0) + (target - acc) + 1;

		/* Defensive: minibuffer_height and `n` both come from
		 * this same capped layout, so the clamp above cannot
		 * fire today and `acc` always matches `li`.  Should
		 * they ever diverge, place the cursor somewhere on
		 * the screen rather than emitting a CUP past its
		 * right edge. */
		if (curs < 1)
			curs = 1;
		if (curs > E.screencols)
			curs = E.screencols;
	}

	snprintf(cbuf, sizeof(cbuf), CSI "%d;%dH", minibuf_row, curs);
	IGNORE_RETURN(write(STDOUT_FILENO, cbuf, strlen(cbuf)));
}

/* Re-measure the terminal and rebuild the layout.*/
void resizeScreen(void) {
	getWindowSize(&E.screenrows, &E.screencols);
	/* Reset window heights so they get recalculated */
	for (int i = 0; i < E.nwindows; i++) {
		E.windows[i]->height = 0;
	}
	computeDisplayNames();
	refreshScreen();
}

/* Handy information about the character under the cursor.*/
void describeChar(void) {
	char info[256];

	if (E.buf->cy >= E.buf->numrows) {
		setStatusMessage("%s", "No line at cursor.");
		return;
	}

	erow *row = &E.buf->row[E.buf->cy];
	int cx = E.buf->cx;

	/* Calculate absolute byte offset from the start of the file/buffer */
	long abs_offset = cx;
	for (int i = 0; i < E.buf->cy; i++) {
		abs_offset += E.buf->row[i].size;
		abs_offset +=
			1; /* +1 for the implicit \n stripped from memory */
	}

	if (cx >= row->size) {
		snprintf(info, sizeof(info),
			 "Character: LINE FEED | Line Byte Offset: %d | "
			 "Absolute Byte Offset: %ld",
			 cx, abs_offset);
		setStatusMessage("%s", info);
		return;
	}

	uint8_t lead = row->chars[cx];
	int byte_len = utf8_nBytes(lead);
	if (byte_len < 1)
		byte_len = 1;
	if (cx + byte_len > row->size)
		byte_len = row->size - cx;

	uint32_t cp = utf8Decode(row->chars, cx);
	int sw = charInStringWidth(row->chars, cx);

	/* Build raw hex bytes string */
	char raw[32] = "";
	int pos = 0;
	for (int i = 0; i < byte_len && pos + 5 < (int)sizeof(raw); i++) {
		pos += snprintf(raw + pos, sizeof(raw) - pos, "0x%02X ",
				row->chars[cx + i]);
	}
	if (pos > 0)
		raw[pos - 1] = '\0';

	/* Get Unicode name for non-printing characters */
	const char *name = unicodeCharName(cp);

	if (name) {
		snprintf(info, sizeof(info),
			 "Character: %s | Unicode Codepoint: U+%04lX | "
			 "Display Width: %d | Line Byte Offset: %d | "
			 "Absolute Byte Offset: %ld | Hex Bytes: %s",
			 name, (unsigned long)cp, sw, cx, abs_offset, raw);
	} else {
		/* Printable character: show the glyph */
		char glyph[8] = { 0 };
		char scriptpart[64] = "";
		const char *script = unicodeScriptName(cp);

		if (script != NULL && script[0] != '\0')
			snprintf(scriptpart, sizeof(scriptpart), " (%s)",
				 script);

		if (byte_len > (int)sizeof(glyph) - 1)
			byte_len = sizeof(glyph) - 1;

		memcpy(glyph, &row->chars[cx], byte_len);

		snprintf(
			info, sizeof(info),
			"Character: %s%s | Unicode Codepoint: U+%04lX | Display Width: %d | Line Byte Offset: %d | Absolute Byte Offset: %ld | Raw Hex Bytes: %s",
			glyph, scriptpart, (unsigned long)cp, sw, cx,
			abs_offset, raw);
	}

	setStatusMessage("%s", info);
}

/* Put the cursor's screen line in the middle of the window by walking
 * back half a window of screen lines from it.*/
void recenter(struct window *win) {
	struct buffer *buf = win->buf;
	int subline = 0;

	if (buf->word_wrap) {
		erow *row = &buf->row[buf->cy];
		int col;
		cursorScreenLine(row, charsToDisplayColumn(row, buf->cx),
				 E.screencols, &subline, &col);
	}

	int row, sub;
	linesBack(buf, buf->cy, subline, win->height / 2, &row, &sub);
	topSet(win, buf, row, sub);
}

void toggleVisualLineMode(void) {
	E.buf->word_wrap = !E.buf->word_wrap;
	setStatusMessage(E.buf->word_wrap ? "Visual line mode enabled" :
					    "Visual line mode disabled");
}

void editorVersion(void) {
	setStatusMessage("emil " EMIL_VERSION);
}

void help(void) {
	setStatusMessage(
		"Open:C-x C-f   Save:C-x C-s   Quit:C-x C-c   "
		"Mark:C-SPC   Kill:C-w   Copy:M-w   Yank:C-y   "
		"Undo:C-_   Search:C-s   Cancel:C-g   "
		"where C- denotes the Control key, M- denotes the Meta (Alt) key.  "
		"For the complete command reference, see the man page.");
}

void setStatusMessage(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_show = 1;
}

void clearStatusMessage(void) {
	E.statusmsg[0] = '\0';
	E.statusmsg_show = 0;
}
