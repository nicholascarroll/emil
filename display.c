#include "display.h"
#include "abuf.h"
#include "emil.h"
#include "message.h"
#include "terminal.h"
#include "unicode.h"
#include "unused.h"
#include "region.h"
#include "buffer.h"
#include "util.h"
#include "window.h"
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <limits.h>
#ifdef __sun
#include <termios.h>
#endif

extern struct editorConfig E;

const int minibuffer_height = 1;
const int statusbar_height = 1;

/* Pre-computed highlight bounds for a single row.  Computed once per row
 * before rendering, then checked with simple integer comparisons in the
 * per-column loop.  This replaces the old isRenderPosInRegion /
 * isRenderPosCurrentSearchMatch calls that each walked the row from byte 0
 * via charsToDisplayColumn up to four times per column. */
struct rowHighlight {
	int region_start; /* first highlighted display column, or -1 */
	int region_end;	  /* one past last highlighted column, or -1 */
	int match_start;  /* search match start column, or -1 */
	int match_end;	  /* search match end column, or -1 */
};

static void computeRowHighlightBounds(struct editorBuffer *buf, int filerow,
				      struct rowHighlight *hl) {
	hl->region_start = -1;
	hl->region_end = -1;
	hl->match_start = -1;
	hl->match_end = -1;

	erow *row = &buf->row[filerow];

	/* Region bounds */
	if (!markInvalidSilent()) {
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
					hl->region_end = INT_MAX;
				} else if (filerow == er) {
					hl->region_start = 0;
					hl->region_end =
						charsToDisplayColumn(row, ec);
				} else {
					/* Middle row: entire row highlighted */
					hl->region_start = 0;
					hl->region_end = INT_MAX;
				}
			}
		}
	}

	/* Search match bounds */
	if (buf->query && buf->query[0] && buf->match && filerow == buf->cy) {
		int match_len = strlen((char *)buf->query);
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

/* Calculate number of rows to scroll for smooth scrolling */
int calculateRowsToScroll(struct editorBuffer *buf, struct editorWindow *win,
			  int direction) {
	int rendered_lines = 0;
	int rows_to_scroll = 0;
	int start_row = (direction > 0) ? win->rowoff : win->rowoff - 1;

	while (rendered_lines < win->height) {
		if (start_row < 0 || start_row >= buf->numrows)
			break;
		erow *row = &buf->row[start_row];
		int line_height = !buf->word_wrap ?
					  1 :
					  countScreenLines(row, E.screencols);
		if (rendered_lines + line_height > win->height && direction < 0)
			break;
		rendered_lines += line_height;
		rows_to_scroll++;
		start_row += direction;
	}

	return rows_to_scroll;
}

/* Render a line with highlighting support.
 *
 * start_col / end_col: the display-column range to render.
 * start_byte: byte offset in row->chars corresponding to start_col,
 *             or -1 to scan from the beginning.  The word-wrap caller
 *             already knows the byte offset; passing it in avoids an
 *             O(line-length) skip loop for every wrapped sub-line. */
static void renderLineWithHighlighting(erow *row, struct abuf *ab,
				       int start_col, int end_col,
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

	/* Render visible portion */
	while (char_idx < row->size && render_x < end_col) {
		uint8_t c = row->chars[char_idx];

		updateHighlight(ab, &current_highlight,
				isHighlighted(hl, render_x) ? 1 : 0);

		if (c == '\t') {
			int next_tab_stop = (render_x + EMIL_TAB_STOP) /
					    EMIL_TAB_STOP * EMIL_TAB_STOP;
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
			render_x += 2;
		} else {
			int width = charInStringWidth(row->chars, char_idx);
			if (render_x >= start_col) {
				int bytes = utf8_nBytes(c);
				abAppend(ab, (char *)&row->chars[char_idx],
					 bytes);
			}
			render_x += width;
		}

		char_idx += utf8_nBytes(row->chars[char_idx]);
	}

	updateHighlight(ab, &current_highlight, 0);
}

/* Display functions */
void setScxScy(struct editorWindow *win) {
	struct editorBuffer *buf = win->buf;
	erow *row = (buf->cy >= buf->numrows) ? NULL : &buf->row[buf->cy];

	win->scy = 0;
	win->scx = 0;

	if (buf->word_wrap) {
		if (buf->cy >= buf->numrows) {
			/* Virtual line past end of buffer */
			if (buf->numrows > 0) {
				int virtual_screen_line = getScreenLineForRow(
					buf, buf->numrows - 1);
				virtual_screen_line += countScreenLines(
					&buf->row[buf->numrows - 1],
					E.screencols);
				int rowoff_screen_line =
					getScreenLineForRow(buf, win->rowoff);
				win->scy = virtual_screen_line -
					   rowoff_screen_line;
			} else {
				win->scy = 0 - win->rowoff;
			}
		} else {
			int cursor_screen_line =
				getScreenLineForRow(buf, buf->cy);
			int rowoff_screen_line =
				getScreenLineForRow(buf, win->rowoff);
			win->scy = cursor_screen_line - rowoff_screen_line;
		}
	} else {
		win->scy = buf->cy - win->rowoff;
	}

	if (buf->cy >= buf->numrows) {
		return;
	}

	int total_width = charsToDisplayColumn(row, buf->cx);

	if (!buf->word_wrap) {
		win->scx = total_width - win->coloff;
	} else {
		int sub_line, sub_col;
		cursorScreenLine(row, total_width, E.screencols, &sub_line,
				 &sub_col);
		win->scy += sub_line;
		win->scx = sub_col;
	}

	if (win->scy < 0)
		win->scy = 0;
	if (win->scy >= win->height)
		win->scy = win->height - 1;
	if (win->scx < 0)
		win->scx = 0;
	if (win->scx >= E.screencols)
		win->scx = E.screencols - 1;
}

void scroll(void) {
	struct editorWindow *win = E.windows[windowFocusedIdx()];
	struct editorBuffer *buf = win->buf;

	if (buf->cy + 1 > buf->numrows) {
		buf->cy = buf->numrows;
		buf->cx = 0;
	} else if (buf->cx > buf->row[buf->cy].size) {
		buf->cx = buf->row[buf->cy].size;
	}

	if (buf->word_wrap) {
		if (buf->cy < win->rowoff) {
			win->rowoff = buf->cy;
		} else {
			int cursor_screen_row = 0;

			for (int i = win->rowoff;
			     i < buf->cy && i < buf->numrows; i++) {
				cursor_screen_row += countScreenLines(
					&buf->row[i], E.screencols);
			}

			if (buf->cy < buf->numrows) {
				int render_pos = charsToDisplayColumn(
					&buf->row[buf->cy], buf->cx);
				int sub_line, sub_col;
				cursorScreenLine(&buf->row[buf->cy], render_pos,
						 E.screencols, &sub_line,
						 &sub_col);
				cursor_screen_row += sub_line;
			}

			if (cursor_screen_row >= win->height) {
				int visible_rows = 0;
				if (buf->cy == buf->numrows) {
					visible_rows = 1;
				}
				for (int i = buf->cy; i >= 0; i--) {
					if (i < buf->numrows) {
						int line_height =
							countScreenLines(
								&buf->row[i],
								E.screencols);
						if (visible_rows + line_height >
						    win->height) {
							win->rowoff = i + 1;
							break;
						}
						visible_rows += line_height;
					}
					if (i == 0) {
						win->rowoff = 0;
						break;
					}
				}
			}
		}
	} else {
		if (buf->cy < win->rowoff) {
			win->rowoff = buf->cy;
		} else if (buf->cy >= win->rowoff + win->height) {
			win->rowoff = buf->cy - win->height + 1;
		}
	}

	if (!buf->word_wrap) {
		int rx = 0;
		if (buf->cy < buf->numrows) {
			rx = charsToDisplayColumn(&buf->row[buf->cy], buf->cx);
		}
		if (rx < win->coloff) {
			win->coloff = rx;
		} else if (rx >= win->coloff + E.screencols) {
			win->coloff = rx - E.screencols + 1;
		}
	} else {
		win->coloff = 0;
	}

	setScxScy(win);
}

/* ================================================================
 * Frame planning and diff-based refresh
 * ================================================================ */

/* Fill a screen line descriptor array for a window without emitting
 * any output.  Returns the number of screen lines filled (always
 * <= screenrows).  The frame[] array must have room for screenrows
 * entries. */
static int planFrame(struct editorWindow *win, struct screenLine *frame,
		     int screenrows, int screencols) {
	struct editorBuffer *buf = win->buf;
	int y = 0;
	int filerow = win->rowoff;

	for (y = 0; y < screenrows; y++) {
		if (filerow >= buf->numrows) {
			/* Empty line past end of buffer */
			frame[y].row = NULL;
			frame[y].gen = 0;
			frame[y].sub_line = 0;
			frame[y].start_byte = 0;
			frame[y].hl_region_s = -1;
			frame[y].hl_region_e = -1;
			frame[y].hl_match_s = -1;
			frame[y].hl_match_e = -1;
		} else {
			erow *row = &buf->row[filerow];
			if (!row->render_valid)
				updateRow(row);

			struct rowHighlight hl;
			computeRowHighlightBounds(buf, filerow, &hl);

			if (!buf->word_wrap) {
				frame[y].row = row;
				frame[y].gen = row->gen;
				frame[y].sub_line = 0;
				frame[y].start_byte = 0;
				frame[y].hl_region_s = hl.region_start;
				frame[y].hl_region_e = hl.region_end;
				frame[y].hl_match_s = hl.match_start;
				frame[y].hl_match_e = hl.match_end;
				filerow++;
			} else {
				int line_start_col = 0;
				int line_start_byte = 0;
				int sub = 0;

				while (line_start_byte < row->size &&
				       y < screenrows) {
					int break_col, break_byte;
					int more = wordWrapBreak(
						row, screencols, line_start_col,
						line_start_byte, &break_col,
						&break_byte);

					frame[y].row = row;
					frame[y].gen = row->gen;
					frame[y].sub_line = sub;
					frame[y].start_byte = line_start_byte;
					frame[y].hl_region_s = hl.region_start;
					frame[y].hl_region_e = hl.region_end;
					frame[y].hl_match_s = hl.match_start;
					frame[y].hl_match_e = hl.match_end;

					if (!more)
						break;
					if (y < screenrows - 1)
						y++;
					else
						break;
					sub++;
					line_start_col = break_col;
					line_start_byte = break_byte;
				}
				filerow++;
			}
		}
	}
	return y;
}

/* Compare two screen line descriptors.  Returns 1 if they represent
 * the same terminal content (no redraw needed). */
static int screenLinesEqual(const struct screenLine *a,
			    const struct screenLine *b) {
	if (a->row != b->row)
		return 0;
	if (a->row == NULL)
		return 1; /* both empty */
	return a->gen == b->gen && a->sub_line == b->sub_line &&
	       a->start_byte == b->start_byte &&
	       a->hl_region_s == b->hl_region_s &&
	       a->hl_region_e == b->hl_region_e &&
	       a->hl_match_s == b->hl_match_s && a->hl_match_e == b->hl_match_e;
}

/* Detect a contiguous vertical scroll between old and new frames.
 * Looks for the longest run where old[i] == new[i + offset].
 * Sets *scroll_offset to the shift (positive = content moved up,
 * i.e. new content appeared at bottom; negative = moved down).
 * Sets *match_start and *match_end to the range in the NEW frame
 * that matches the shifted old content (inclusive start, exclusive end).
 * Returns the length of the matching run, or 0 if no useful scroll
 * was found. */
static int detectScroll(const struct screenLine *old_frame, int old_count,
			const struct screenLine *new_frame, int new_count,
			int *scroll_offset, int *match_start, int *match_end) {
	/* Try small offsets first — most scrolls are by 1-3 lines.
	 * We check offsets ±1 through ±(height/2). */
	int max_offset = new_count / 2;
	int best_run = 0;
	int best_offset = 0;
	int best_start = 0;

	for (int off = -max_offset; off <= max_offset; off++) {
		if (off == 0)
			continue;

		/* Find the longest contiguous run where
		 * old[i] == new[i + off] */
		int run = 0;
		int run_start = -1;
		int longest = 0;
		int longest_start = 0;

		/* i indexes old frame, i+off indexes new frame */
		int i_start = (off > 0) ? 0 : -off;
		int i_end = (off > 0) ? (old_count - off) : old_count;
		if (i_end > old_count)
			i_end = old_count;
		if (i_start + off + 0 >= new_count)
			continue;

		for (int i = i_start; i < i_end; i++) {
			int ni = i + off;
			if (ni < 0 || ni >= new_count) {
				if (run > longest) {
					longest = run;
					longest_start = run_start;
				}
				run = 0;
				continue;
			}
			if (screenLinesEqual(&old_frame[i], &new_frame[ni])) {
				if (run == 0)
					run_start = ni;
				run++;
			} else {
				if (run > longest) {
					longest = run;
					longest_start = run_start;
				}
				run = 0;
			}
		}
		if (run > longest) {
			longest = run;
			longest_start = run_start;
		}

		if (longest > best_run) {
			best_run = longest;
			best_offset = off;
			best_start = longest_start;
		}
	}

	/* Only use scroll if it saves significant work — at least
	 * half the screen lines can be preserved. */
	if (best_run >= new_count / 2 && best_run >= 3) {
		*scroll_offset = best_offset;
		*match_start = best_start;
		*match_end = best_start + best_run;
		return best_run;
	}

	*scroll_offset = 0;
	*match_start = 0;
	*match_end = 0;
	return 0;
}

/* Draw a single screen line at terminal row term_row (1-based).
 * Uses the screen line descriptor to determine what to render. */
static void drawSingleRow(struct editorWindow *win, struct abuf *ab,
			  const struct screenLine *sl, int term_row,
			  int screencols) {
	char pos[32];
	snprintf(pos, sizeof(pos), CSI "%d;1H", term_row);
	abAppend(ab, pos, strlen(pos));

	if (sl->row == NULL) {
		abAppend(ab, " ", 1);
	} else {
		struct rowHighlight hl;
		hl.region_start = sl->hl_region_s;
		hl.region_end = sl->hl_region_e;
		hl.match_start = sl->hl_match_s;
		hl.match_end = sl->hl_match_e;

		if (!win->buf->word_wrap) {
			renderLineWithHighlighting(sl->row, ab, win->coloff,
						   win->coloff + screencols,
						   &hl, -1);
		} else {
			/* Find the end of this sub-line by running
			 * wordWrapBreak from start_byte. */
			int break_col, break_byte;
			int line_start_col = 0;

			/* We need line_start_col. Compute it from
			 * start_byte by walking from byte 0.  For
			 * sub_line 0 it's always 0. */
			if (sl->sub_line == 0) {
				line_start_col = 0;
			} else {
				line_start_col = charsToDisplayColumn(
					sl->row, sl->start_byte);
			}

			wordWrapBreak(sl->row, screencols, line_start_col,
				      sl->start_byte, &break_col, &break_byte);

			renderLineWithHighlighting(sl->row, ab, line_start_col,
						   break_col, &hl,
						   sl->start_byte);

			/* Fill trailing space */
			int fill_col = break_col;
			int fill_hl = 0;
			while (fill_col - line_start_col < screencols) {
				updateHighlight(
					ab, &fill_hl,
					isHighlighted(&hl, fill_col) ? 1 : 0);
				abAppend(ab, " ", 1);
				fill_col++;
			}
			updateHighlight(ab, &fill_hl, 0);
		}
	}
	abAppend(ab, "\x1b[K", 3); /* clear to end of line */
}

void drawRows(struct editorWindow *win, struct abuf *ab, int screenrows,
	      int screencols) {
	struct editorBuffer *buf = win->buf;
	int y;
	int filerow = win->rowoff;

	for (y = 0; y < screenrows; y++) {
		if (filerow >= buf->numrows) {
			abAppend(ab, " ", 1);
		} else {
			erow *row = &buf->row[filerow];
			if (!row->render_valid) {
				updateRow(row);
			}
			if (!buf->word_wrap) {
				// Truncated mode with visual marking
				struct rowHighlight hl;
				computeRowHighlightBounds(buf, filerow, &hl);
				renderLineWithHighlighting(
					row, ab, win->coloff,
					win->coloff + screencols, &hl, -1);
				filerow++;
			} else {
				/* Word-wrap mode: break lines at word
				 * boundaries when possible. */
				int line_start_col = 0;
				int line_start_byte = 0;

				struct rowHighlight hl;
				computeRowHighlightBounds(buf, filerow, &hl);

				while (line_start_byte < row->size &&
				       y < screenrows) {
					int break_col, break_byte;
					int more = wordWrapBreak(
						row, screencols, line_start_col,
						line_start_byte, &break_col,
						&break_byte);

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

					/* --- Advance to next screen line
					 *     if more content remains --- */
					if (more && y < screenrows - 1) {
						abAppend(ab, "\r\n", 2);
						y++;
					}

					if (!more)
						break;
					line_start_col = break_col;
					line_start_byte = break_byte;
				}

				filerow++;
			}
		}
		abAppend(ab, "\x1b[K", 3);
		if (y < screenrows - 1) {
			abAppend(ab, "\r\n", 2);
		}
	}
}

void drawStatusBar(struct editorWindow *win, struct abuf *ab, int line) {
	/* XXX: It's actually possible for the status bar to end up
	 * outside where it should be, so set it explicitly. */
	char buf[32];
	snprintf(buf, sizeof(buf), CSI "%d;%dH", line, 1);
	abAppend(ab, buf, strlen(buf));

	struct editorBuffer *bufr = win->buf;

	abAppend(ab, "\x1b[7m", 4);
	char status[80];
	int len = 0;
	if (win->focused) {
		len = snprintf(status, sizeof(status),
			       "-- %.20s %c%c%c %2d:%2d --",
			       bufr->filename ? bufr->filename : "*scratch*",
			       bufr->dirty ? '*' : '-', bufr->dirty ? '*' : '-',
			       bufr->read_only ? '%' : ' ', bufr->cy + 1,
			       bufr->cx);
	} else {
		len = snprintf(status, sizeof(status),
			       "   %.20s %c%c%c %2d:%2d   ",
			       bufr->filename ? bufr->filename : "*scratch*",
			       bufr->dirty ? '*' : '-', bufr->dirty ? '*' : '-',
			       bufr->read_only ? '%' : ' ', win->cy + 1,
			       win->cx);
	}
#ifdef EMIL_DEBUG_UNDO
#ifdef EMIL_DEBUG_REDO
#define DEBUG_UNDO bufr->redo
#else
#define DEBUG_UNDO bufr->undo
#endif
	if (DEBUG_UNDO != NULL) {
		len = 0;
		for (len = 0; len < DEBUG_UNDO->datalen; len++) {
			status[len] = DEBUG_UNDO->data[len];
			if (DEBUG_UNDO->data[len] == '\n')
				status[len] = '#';
		}
		status[len++] = '"';
		len += snprintf(&status[len], sizeof(status) - len,
				"sx %d sy %d ex %d ey %d cx %d cy %d",
				DEBUG_UNDO->startx, DEBUG_UNDO->starty,
				DEBUG_UNDO->endx, DEBUG_UNDO->endy, bufr->cx,
				bufr->cy);
	}
#endif
#ifdef EMIL_DEBUG_MACROS
	/* This can get quite wide, you may want to boost the size of status */
	for (int i = 0; i < E.macro.nkeys; i++) {
		len += snprintf(&status[len], sizeof(status) - len, "%d: %d ",
				i, E.macro.keys[i]);
	}
#endif

	char perc[8];
	if (bufr->numrows == 0)
		memcpy(perc, " Emp --", 7);
	else if (bufr->end && win->rowoff == 0)
		memcpy(perc, " All --", 7);
	else if (bufr->end)
		memcpy(perc, " Bot --", 7);
	else if (win->rowoff == 0)
		memcpy(perc, " Top --", 7);
	else
		snprintf(perc, sizeof(perc), " %2d%% --",
			 (win->rowoff * 100) / bufr->numrows);

	if (!win->focused) {
		perc[5] = ' ';
		perc[6] = ' ';
	}

	if (len > E.screencols)
		len = E.screencols;
	abAppend(ab, status, len);

	/* Fill the gap between status text and percentage indicator.
	 * Use a CSI column-jump to skip to the percentage position
	 * rather than emitting one fill byte at a time. */
	int perc_col = E.screencols - 7 + 1; /* 1-based column */
	if (len < E.screencols - 7) {
		if (win->focused) {
			/* Emit a run of dashes to fill the gap.
			 * For very wide terminals, use a CSI jump
			 * followed by just enough dashes. */
			int fill_count = E.screencols - 7 - len;
			if (fill_count <= 256) {
				char fill_buf[256];
				memset(fill_buf, '-', fill_count);
				abAppend(ab, fill_buf, fill_count);
			} else {
				/* Jump to the percentage column and
				 * let reverse-video fill the gap */
				char jump[16];
				int jlen = snprintf(jump, sizeof(jump),
						    CSI "%dG", perc_col);
				abAppend(ab, jump, jlen);
			}
		} else {
			/* Unfocused: jump directly, spaces are already the
			 * background under reverse video */
			char jump[16];
			int jlen = snprintf(jump, sizeof(jump), CSI "%dG",
					    perc_col);
			abAppend(ab, jump, jlen);
		}
	}
	abAppend(ab, perc, 7);
	abAppend(ab, "\x1b[m" CRLF, 5);
}

void drawMinibuffer(struct abuf *ab) {
	abAppend(ab, "\x1b[K", 3);

	// Show prefix first if active
	if (E.prefix_display[0]) {
		abAppend(ab, E.prefix_display, strlen(E.prefix_display));
	}

	// Then show message
	int msglen = strlen(E.statusmsg);
	if (msglen > E.screencols)
		msglen = E.screencols;
	if (msglen && time(NULL) - E.statusmsg_time < 5) {
		if (E.buf->query && !E.buf->match) {
			abAppend(ab, "\x1b[91m", 5);
		}
		abAppend(ab, E.statusmsg, msglen);
		abAppend(ab, "\x1b[0m", 4);
	}
}

void refreshScreen(void) {
	int focusedIdx = windowFocusedIdx();

	int total_height = E.screenrows - minibuffer_height -
			   (statusbar_height * E.nwindows);

	/* Ensure window heights are set */
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
			struct editorWindow *win = E.windows[i];
			win->height = window_height;
			if (i == E.nwindows - 1)
				win->height += remaining_height;
		}
	}

	/* Run scroll for focused window */
	if (E.windows[focusedIdx]->focused)
		scroll();

	struct abuf ab = ABUF_INIT;
	abAppend(&ab, "\x1b[?25l", 6); /* hide cursor */

	int cumulative_height = 0;

	for (int w = 0; w < E.nwindows; w++) {
		struct editorWindow *win = E.windows[w];
		int win_top = cumulative_height + 1; /* 1-based terminal row */
		int win_height = win->height;

		/* Plan new frame */
		struct screenLine *new_frame =
			xmalloc(sizeof(struct screenLine) * win_height);
		planFrame(win, new_frame, win_height, E.screencols);

		struct screenLine *old_frame = win->prev_frame;
		int old_count =
			(old_frame && win->prev_frame_size >= win_height) ?
				win_height :
				0;

		/* Invalidate old frame if coloff changed (truncated mode
		 * horizontal scroll changes everything) */
		if (win->coloff != win->prev_coloff)
			old_count = 0;

		/* Detect scroll */
		int scroll_offset = 0, match_start = 0, match_end = 0;
		int scroll_run = 0;
		if (old_count > 0) {
			scroll_run = detectScroll(old_frame, old_count,
						  new_frame, win_height,
						  &scroll_offset, &match_start,
						  &match_end);
		}

		if (scroll_run > 0 && scroll_offset != 0) {
			/* Use DECSTBM to scroll the matched region */
			int region_top = win_top + match_start;
			int region_bot = win_top + match_end - 1;
			int lines = scroll_offset > 0 ? scroll_offset :
							-scroll_offset;
			char seq[32];

			/* Set scroll region */
			snprintf(seq, sizeof(seq), CSI "%d;%dr", region_top,
				 region_bot);
			abAppend(&ab, seq, strlen(seq));

			if (scroll_offset > 0) {
				/* Content moved up: new lines at bottom.
				 * Position at bottom of region, emit \n */
				snprintf(seq, sizeof(seq), CSI "%dH",
					 region_bot);
				abAppend(&ab, seq, strlen(seq));
				for (int i = 0; i < lines; i++)
					abAppend(&ab, "\n", 1);
			} else {
				/* Content moved down: new lines at top.
				 * Position at top of region, emit RI */
				snprintf(seq, sizeof(seq), CSI "%dH",
					 region_top);
				abAppend(&ab, seq, strlen(seq));
				for (int i = 0; i < lines; i++)
					abAppend(&ab, ESC "M", 2);
			}

			/* Reset scroll region */
			abAppend(&ab, CSI "r", 3);

			/* Draw only the lines that weren't preserved
			 * by the scroll */
			for (int y = 0; y < win_height; y++) {
				if (y >= match_start && y < match_end)
					continue; /* preserved by scroll */
				drawSingleRow(win, &ab, &new_frame[y],
					      win_top + y, E.screencols);
			}
		} else if (old_count > 0) {
			/* No scroll — diff line by line */
			for (int y = 0; y < win_height; y++) {
				if (y < old_count &&
				    screenLinesEqual(&old_frame[y],
						     &new_frame[y]))
					continue; /* unchanged */
				drawSingleRow(win, &ab, &new_frame[y],
					      win_top + y, E.screencols);
			}
		} else {
			/* No previous frame — full redraw.
			 * Position to window top and draw all rows. */
			char pos[16];
			snprintf(pos, sizeof(pos), CSI "%dH", win_top);
			abAppend(&ab, pos, strlen(pos));
			drawRows(win, &ab, win_height, E.screencols);
		}

		/* Save frame for next refresh */
		if (win->prev_frame_size < win_height) {
			free(win->prev_frame);
			win->prev_frame =
				xmalloc(sizeof(struct screenLine) * win_height);
			win->prev_frame_size = win_height;
		}
		memcpy(win->prev_frame, new_frame,
		       sizeof(struct screenLine) * win_height);
		win->prev_coloff = win->coloff;
		free(new_frame);

		cumulative_height += win_height + statusbar_height;
		drawStatusBar(win, &ab, cumulative_height);
	}

	drawMinibuffer(&ab);
	abAppend(&ab, "\x1b[J", 3); /* clear below */

	/* Position cursor */
	struct editorWindow *focusedWin = E.windows[focusedIdx];
	char buf[32];
	int cursor_y = focusedWin->scy + 1;
	for (int i = 0; i < focusedIdx; i++)
		cursor_y += E.windows[i]->height + statusbar_height;

	int cumul_check = 0;
	for (int i = 0; i < E.nwindows; i++)
		cumul_check += E.windows[i]->height + statusbar_height;
	if (cursor_y > cumul_check) {
		struct editorBuffer *b = focusedWin->buf;
		cursor_y = (b->cy >= b->numrows) ?
				   cumul_check :
				   cumul_check - statusbar_height;
	}

	snprintf(buf, sizeof(buf), CSI "%d;%dH", cursor_y, focusedWin->scx + 1);
	abAppend(&ab, buf, strlen(buf));
	abAppend(&ab, "\x1b[?25h", 6); /* show cursor */

	write(STDOUT_FILENO, ab.b, ab.len);
	abFree(&ab);
}

void cursorBottomLine(int curs) {
	char cbuf[32];
	/* Calculate actual minibuffer row position */
	int minibuf_row = 0;
	for (int i = 0; i < E.nwindows; i++) {
		minibuf_row += E.windows[i]->height + statusbar_height;
	}
	minibuf_row++; /* minibuffer is after all windows/status bars */
	snprintf(cbuf, sizeof(cbuf), CSI "%d;%dH", minibuf_row, curs);
	write(STDOUT_FILENO, cbuf, strlen(cbuf));
}

void editorResizeScreen(int UNUSED(sig)) {
	if (getWindowSize(&E.screenrows, &E.screencols) == -1)
		die("getWindowSize");
	refreshScreen();
}

void editorWhatCursor(void) {
	int rx = 0;
	int line_len = 0;
	if (E.buf->cy < E.buf->numrows) {
		erow *row = &E.buf->row[E.buf->cy];
		line_len = row->size;
		rx = charsToDisplayColumn(row, E.buf->cx);
	}

	/* Get character at cursor */
	char ch[8] = "EOL";
	if (E.buf->cy < E.buf->numrows &&
	    E.buf->cx < E.buf->row[E.buf->cy].size) {
		uint8_t c = E.buf->row[E.buf->cy].chars[E.buf->cx];
		if (c < 32) {
			snprintf(ch, sizeof(ch), "^%c", c + 64);
		} else if (c == 127) {
			snprintf(ch, sizeof(ch), "^?");
		} else if (c < 128) {
			snprintf(ch, sizeof(ch), "%c", c);
		} else {
			snprintf(ch, sizeof(ch), "\\x%02X", c);
		}
	}

	int screen_y = E.buf->cy - E.windows[0]->rowoff + 1;
	editorSetStatusMessage(
		"Line,col (buffer:%d,%d screen:%d,%d) Char='%s' LineLen=%d Window=%dx%d",
		E.buf->cy + 1, E.buf->cx, screen_y, rx, ch, line_len,
		E.screencols, E.screenrows);
}

void recenter(struct editorWindow *win) {
	win->rowoff = win->buf->cy - (win->height / 2);
	if (win->rowoff < 0) {
		win->rowoff = 0;
	}
}

void editorToggleVisualLineMode(void) {
	E.buf->word_wrap = !E.buf->word_wrap;
	editorSetStatusMessage(E.buf->word_wrap ? "Visual line mode enabled" :
						  "Visual line mode disabled");
}

void editorVersion(void) {
	editorSetStatusMessage("emil version " EMIL_VERSION);
}

/* Wrapper for command table */
void editorVersionWrapper(struct editorConfig *UNUSED(ed),
			  struct editorBuffer *UNUSED(buf)) {
	editorVersion();
}

/* Wrapper for command table */
void editorToggleVisualLineModeWrapper(struct editorConfig *UNUSED(ed),
				       struct editorBuffer *UNUSED(buf)) {
	editorToggleVisualLineMode();
}
