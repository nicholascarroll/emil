#include "display.h"
#include "abuf.h"
#include "emil.h"
#include "fileio.h"
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

int minibuffer_height = 1;
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
		hl->region_end = charsToDisplayColumn(row, row->size);
		return;
	}

	/* Region bounds — only highlight when mark is active */
	if (buf->mark_active && !markInvalidSilent()) {
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

/* Scroll the viewport by `n` screen lines.  Positive = down (content
 * moves up), negative = up (content moves down).  Handles both wrap
 * and non-wrap modes, managing rowoff and skip_sublines.
 *
 * This is a pure viewport operation — it does NOT touch the cursor.
 * Callers are responsible for adjusting the cursor afterwards. */
void scrollViewport(struct editorWindow *win, struct editorBuffer *buf, int n) {
	if (n == 0)
		return;

	if (!buf->word_wrap) {
		win->rowoff += n;
		if (win->rowoff < 0)
			win->rowoff = 0;
		int max_rowoff = buf->numrows - win->height + 2;
		if (max_rowoff < 0)
			max_rowoff = 0;
		if (buf->numrows > 0 && win->rowoff > max_rowoff)
			win->rowoff = max_rowoff;
		win->skip_sublines = 0;
		return;
	}

	/* Word-wrap mode: scroll by individual screen lines */
	buildScreenCache(buf, E.screencols);

	if (n > 0) {
		/* Scroll down */
		int last = buf->numrows - 1;
		int last_end = getScreenLineForRow(buf, last, E.screencols) +
			       countScreenLines(&buf->row[last], E.screencols);

		for (int i = 0; i < n; i++) {
			if (win->rowoff >= buf->numrows)
				break;

			/* Stop if the last buffer line is already visible
			 * in the window (with a couple of blank lines). */
			int top = getScreenLineForRow(buf, win->rowoff,
						      E.screencols) +
				  win->skip_sublines;
			if (last_end <= top + win->height - 2)
				break;

			int row_lines = countScreenLines(&buf->row[win->rowoff],
							 E.screencols);

			if (win->skip_sublines < row_lines - 1) {
				win->skip_sublines++;
			} else {
				win->rowoff++;
				win->skip_sublines = 0;
				if (win->rowoff >= buf->numrows)
					break;
			}
		}
	} else {
		/* Scroll up */
		int up = -n;
		for (int i = 0; i < up; i++) {
			if (win->rowoff <= 0 && win->skip_sublines <= 0)
				break;

			if (win->skip_sublines > 0) {
				win->skip_sublines--;
			} else {
				win->rowoff--;
				int prev_lines = countScreenLines(
					&buf->row[win->rowoff], E.screencols);
				win->skip_sublines = prev_lines - 1;
			}
		}
	}
}

/* Update buf->end: is the last buffer line visible in the window? */
static void updateEndFlag(struct editorWindow *win, struct editorBuffer *buf) {
	if (buf->numrows == 0) {
		buf->end = 1;
		return;
	}
	if (!buf->word_wrap) {
		buf->end = (win->rowoff + win->height > buf->numrows);
		return;
	}
	buildScreenCache(buf, E.screencols);
	int last = buf->numrows - 1;
	int last_end = getScreenLineForRow(buf, last, E.screencols) +
		       countScreenLines(&buf->row[last], E.screencols);
	int top = getScreenLineForRow(buf, win->rowoff, E.screencols) +
		  win->skip_sublines;
	buf->end = (last_end <= top + win->height);
}

/* Return the absolute screen line for the top of the current viewport,
 * accounting for skip_sublines. */
static int viewportTopScreenLine(struct editorWindow *win,
				 struct editorBuffer *buf) {
	return getScreenLineForRow(buf, win->rowoff, E.screencols) +
	       win->skip_sublines;
}

/* Ensure the cursor is within the visible viewport.  If it has fallen
 * outside, drag it to the nearest visible row.  Pure cursor fixup —
 * does not touch the viewport. */
void clampCursorToViewport(struct editorWindow *win, struct editorBuffer *buf) {
	if (!buf->word_wrap) {
		if (buf->cy < win->rowoff)
			buf->cy = win->rowoff;
		else if (buf->cy >= win->rowoff + win->height)
			buf->cy = win->rowoff + win->height - 1;
	} else {
		buildScreenCache(buf, E.screencols);
		int top = viewportTopScreenLine(win, buf);

		/* Handle cursor past EOF */
		if (buf->numrows > 0 && buf->cy >= buf->numrows)
			buf->cy = buf->numrows - 1;

		int cursor_screen =
			getScreenLineForRow(buf, buf->cy, E.screencols);

		if (cursor_screen < top) {
			/* Cursor above viewport — move down */
			while (buf->cy < buf->numrows - 1) {
				buf->cy++;
				cursor_screen = getScreenLineForRow(
					buf, buf->cy, E.screencols);
				if (cursor_screen >= top)
					break;
			}
			buf->cx = 0;
		} else if (cursor_screen >= top + win->height) {
			/* Cursor below viewport — move up */
			while (buf->cy > 0) {
				cursor_screen = getScreenLineForRow(
					buf, buf->cy, E.screencols);
				if (cursor_screen < top + win->height)
					break;
				buf->cy--;
			}
		}
	}

	if (buf->cy < 0)
		buf->cy = 0;
	if (buf->cy < buf->numrows && buf->cx > buf->row[buf->cy].size)
		buf->cx = buf->row[buf->cy].size;
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
					buf, buf->numrows - 1, E.screencols);
				virtual_screen_line += countScreenLines(
					&buf->row[buf->numrows - 1],
					E.screencols);
				int rowoff_screen_line = getScreenLineForRow(
					buf, win->rowoff, E.screencols);
				win->scy = virtual_screen_line -
					   rowoff_screen_line -
					   win->skip_sublines;
			} else {
				win->scy = 0 - win->rowoff;
			}
		} else {
			int cursor_screen_line =
				getScreenLineForRow(buf, buf->cy, E.screencols);
			int rowoff_screen_line = getScreenLineForRow(
				buf, win->rowoff, E.screencols);
			win->scy = cursor_screen_line - rowoff_screen_line -
				   win->skip_sublines;
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
	if (buf->word_wrap && win->scx >= E.screencols) {
		win->scy++;
		win->scx = 0;
	}
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
		/* Ensure cache is built (it should already be by refreshScreen) */
		buildScreenCache(buf, E.screencols);

		/* Compute cursor's absolute screen line position.
		 * When cy == numrows (virtual line past EOF), the
		 * cursor is one screen line past the last row. */
		int cursor_screen_line;
		int cursor_sub_line = 0;
		if (buf->cy >= buf->numrows) {
			if (buf->numrows > 0) {
				cursor_screen_line =
					getScreenLineForRow(buf,
							    buf->numrows - 1,
							    E.screencols) +
					countScreenLines(
						&buf->row[buf->numrows - 1],
						E.screencols);
			} else {
				cursor_screen_line = 0;
			}
		} else {
			cursor_screen_line =
				getScreenLineForRow(buf, buf->cy, E.screencols);
			int render_pos = charsToDisplayColumn(
				&buf->row[buf->cy], buf->cx);
			int sub_col;
			cursorScreenLine(&buf->row[buf->cy], render_pos,
					 E.screencols, &cursor_sub_line,
					 &sub_col);
			cursor_screen_line += cursor_sub_line;
		}

		int rowoff_screen_line =
			getScreenLineForRow(buf, win->rowoff, E.screencols);
		/* Account for current skip_sublines in the effective
		 * top-of-window position */
		int effective_top = rowoff_screen_line + win->skip_sublines;

		if (cursor_screen_line < effective_top) {
			/* Cursor above window: snap rowoff to cursor row */
			win->rowoff = buf->cy;
			win->skip_sublines = cursor_sub_line;
		} else if (cursor_screen_line >= effective_top + win->height) {
			/* Cursor below window: find new rowoff using
			 * cache-based target_top search (§5.3) */
			int target_top = cursor_screen_line - win->height + 1;

			/* Walk backwards from cursor row to find
			 * rowoff where screen_line_start[rowoff]
			 * <= target_top */
			int r = buf->cy;
			if (r >= buf->numrows)
				r = buf->numrows - 1;
			while (r > 0 &&
			       getScreenLineForRow(buf, r, E.screencols) >
				       target_top)
				r--;
			win->rowoff = r;
			win->skip_sublines =
				target_top -
				getScreenLineForRow(buf, r, E.screencols);
		}
		/* Otherwise: cursor is visible, keep rowoff and
		 * skip_sublines as they are */
	} else {
		/* Reset skip_sublines in non-wrap mode */
		win->skip_sublines = 0;

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

void drawRows(struct editorWindow *win, struct abuf *ab, int screenrows,
	      int screencols) {
	struct editorBuffer *buf = win->buf;
	int y;
	int filerow = win->rowoff;
	int skip = win->skip_sublines; /* sub-lines to skip on first row */

	for (y = 0; y < screenrows; y++) {
		int filled =
			0; /* set when word-wrap fill loop padded this line */
		if (filerow >= buf->numrows) {
			abAppend(ab, " ", 1);
		} else {
			erow *row = &buf->row[filerow];
			if (!buf->word_wrap) {
				// Truncated mode with visual marking
				struct rowHighlight hl;
				computeRowHighlightBounds(buf, filerow, &hl);
				renderLineWithHighlighting(
					row, ab, win->coloff,
					win->coloff + screencols, &hl, -1);
				/* Pad remainder of screen line so \x1b[K
				 * is not needed (it would erase the last
				 * column due to pending-wrap state). */
				int rx = charsToDisplayColumn(row, row->size) -
					 win->coloff;
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

				struct rowHighlight hl;
				computeRowHighlightBounds(buf, filerow, &hl);

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
						sub_line_idx++;
						if (!more)
							break;
						line_start_col = break_col;
						line_start_byte = break_byte;
						continue;
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

					/* --- Advance to next screen line
					 *     if more content remains --- */
					if (more && y < screenrows - 1) {
						abAppend(ab, "\r\n", 2);
						y++;
					}

					if (!more)
						break;
					/* Window full — stop rendering this
					 * row to avoid overflowing into the
					 * status bar / next window area. */
					if (y >= screenrows - 1)
						break;
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
}

void drawStatusBar(struct editorWindow *win, struct abuf *ab, int line) {
	char buf[32];
	snprintf(buf, sizeof(buf), CSI "%d;%dH", line, 1);
	abAppend(ab, buf, strlen(buf));

	struct editorBuffer *bufr = win->buf;
	abAppend(ab, "\x1b[7m", 4);

	int total = E.screencols;
	int focused = win->focused;
	char fc = focused ? ' ' : '-';
	const char *sep = focused ? "  " : "--";
	const char *prefix = focused ? "" : "";
	int prefix_len = strlen(prefix);

	/* Prepare content */
	const char *dname =
		bufr->display_name ?
			bufr->display_name :
			(bufr->filename ? bufr->filename : "*scratch*");
	int dlen = strlen(dname);
	const char *bname = strrchr(dname, '/');
	bname = bname ? bname + 1 : dname;
	int bname_len = strlen(bname);

	/* Minimum name width: must show full basename (plus 3 for "..."
	 * if display_name has a path prefix), or disambiguation length,
	 * whichever is larger. */
	int has_path = (dlen > bname_len);
	int min_name = has_path ? bname_len + 3 : bname_len;
	if (bufr->min_name_len > min_name)
		min_name = bufr->min_name_len;
	if (min_name > dlen)
		min_name = dlen;

	char flags[8];
	const char *mod_flag = bufr->external_mod ? "!" : "";
	snprintf(flags, sizeof(flags), "%c%c%c%s", bufr->dirty ? '*' : '-',
		 bufr->dirty ? '*' : '-', bufr->read_only ? '%' : ' ',
		 mod_flag);
	int flags_len = strlen(flags);

	int ry = focused ? bufr->cy + 1 : win->cy + 1;
	int rx = focused ? bufr->cx : win->cx;
	char linecol[24];
	int linecol_len =
		snprintf(linecol, sizeof(linecol), "%s%d:%d", sep, ry, rx);

	char pos[8];
	if (bufr->numrows == 0)
		memcpy(pos, "Emp", 4);
	else if (bufr->end && win->rowoff == 0)
		memcpy(pos, "All", 4);
	else if (bufr->end)
		memcpy(pos, "Bot", 4);
	else if (win->rowoff == 0)
		memcpy(pos, "Top", 4);
	else
		snprintf(pos, sizeof(pos), "%2d%%",
			 (win->rowoff * 100) / bufr->numrows);

	const char *paren = NULL;
	if (E.recording && bufr->word_wrap)
		paren = "(Macro Wrap)";
	else if (E.recording)
		paren = "(Macro)";
	else if (bufr->word_wrap)
		paren = "(Wrap)";

	/* Layout: three 15-char blocks.
	 *   Block 1 (LHS): prefix + name + space + flags
	 *   Block 2 (mid): linecol + pos indicator
	 *   Block 3 (RHS): parenthetical
	 *
	 * Block 1 gets at least min_name chars for the name.
	 * Blocks 2 and 3 are each added only if a full 15 chars
	 * remain after securing the name. Leftover width goes
	 * back to the name. */
	const int BLOCK = 15;
	int name_need = prefix_len + 1 + flags_len + min_name;
	int remain = total - name_need;

	int have_mid = (remain >= BLOCK);
	if (have_mid)
		remain -= BLOCK;

	int have_rhs = (remain >= BLOCK);
	if (have_rhs)
		remain -= BLOCK;

	int name_width = min_name + (remain > 0 ? remain : 0);
	/* Clamp so the name never pushes Block 1 past total width. */
	int max_name = total - prefix_len - 1 - flags_len;
	if (max_name < 1)
		max_name = 1;
	if (name_width > max_name)
		name_width = max_name;

	/* Truncate display name to fit name_width.
	 * Left-truncate with "...", keeping the basename end. */
	const char *show_name = dname;
	char trunc[256];
	if (dlen > name_width) {
		int tail = name_width - 3;
		if (tail < 1)
			tail = 1;
		snprintf(trunc, sizeof(trunc), "...%s", dname + dlen - tail);
		show_name = trunc;
	}

	/* Block 1: LHS */
	char left[512];
	int left_len = snprintf(left, sizeof(left), "%s%s %s", prefix,
				show_name, flags);

	/* Block 2: linecol (left) + pos (right), padded to BLOCK */
	char mid[16];
	int mid_len = 0;
	if (have_mid) {
		int lc = linecol_len < BLOCK ? linecol_len : BLOCK;
		int pos_len = strlen(pos);
		int gap = BLOCK - lc - pos_len;

		memcpy(mid, linecol, lc);
		mid_len = lc;
		if (gap > 0) {
			memset(mid + mid_len, fc, gap);
			mid_len += gap;
		}
		if (mid_len + pos_len <= BLOCK) {
			memcpy(mid + mid_len, pos, pos_len);
			mid_len += pos_len;
		}
		while (mid_len < BLOCK)
			mid[mid_len++] = fc;
	}

	/* Block 3: paren right-aligned, padded to BLOCK */
	char rhs[16];
	int rhs_len = 0;
	if (have_rhs) {
		if (paren) {
			int clen =
				snprintf(rhs, sizeof(rhs), "%s%s", sep, paren);
			int pad = BLOCK - clen;
			if (pad > 0) {
				memmove(rhs + pad, rhs, clen);
				memset(rhs, fc, pad);
			}
			rhs_len = BLOCK;
		} else {
			memset(rhs, fc, BLOCK);
			rhs_len = BLOCK;
		}
	}

	/* Write: LHS + fill + mid + RHS */
	abAppend(ab, left, left_len);

	int gap = total - left_len - mid_len - rhs_len;
	while (gap-- > 0)
		abAppend(ab, &fc, 1);

	if (mid_len > 0)
		abAppend(ab, mid, mid_len);
	if (rhs_len > 0)
		abAppend(ab, rhs, rhs_len);

	abAppend(ab, "\x1b[m" CRLF, 5);
}
void drawMinibuffer(struct abuf *ab) {
	/* Determine the message to display */
	const char *msg = E.statusmsg;
	int msglen = strlen(msg);
	int valid = msglen && time(NULL) - E.statusmsg_time < 5;

	/* Prefix takes space on the first line */
	int prefix_len = strlen(E.prefix_display);

	/* Draw each minibuffer line */
	for (int line = 0; line < minibuffer_height; line++) {
		abAppend(ab, "\x1b[K", 3); /* clear line */

		if (!valid) {
			/* No message — just emit blank lines */
			if (line < minibuffer_height - 1)
				abAppend(ab, "\r\n", 2);
			continue;
		}

		int offset;
		if (line == 0) {
			/* First line: prefix + start of message */
			if (prefix_len > 0)
				abAppend(ab, E.prefix_display, prefix_len);
			offset = 0;
		} else {
			/* Continuation lines: message continues */
			offset = (E.screencols - prefix_len) +
				 (line - 1) * E.screencols;
		}

		int avail = (line == 0) ? E.screencols - prefix_len :
					  E.screencols;
		if (avail < 0)
			avail = 0;

		if (offset < msglen) {
			int chunk = msglen - offset;
			if (chunk > avail)
				chunk = avail;
			if (E.buf->query && !E.buf->match)
				abAppend(ab, "\x1b[91m", 5);
			abAppend(ab, msg + offset, chunk);
			abAppend(ab, "\x1b[0m", 4);
		}

		if (line < minibuffer_height - 1)
			abAppend(ab, "\r\n", 2);
	}
}

void refreshScreen(void) {
	/* Check for external modification of the focused buffer's file */
	editorCheckFileModified(E.buf);

	struct abuf ab = ABUF_INIT;
	abAppend(&ab, "\x1b[?7l", 5);  // Disable auto-wrap
	abAppend(&ab, "\x1b[?25l", 6); // Hide cursor
	abAppend(&ab, "\x1b[H", 3);    // Move cursor to top-left corner

	/* Mandatory bounds clamp for all windows (§7.2) */
	for (int i = 0; i < E.nwindows; i++) {
		struct editorWindow *w = E.windows[i];
		struct editorBuffer *b = w->buf;
		if (b->numrows == 0) {
			w->rowoff = 0;
			w->skip_sublines = 0;
		} else if (w->rowoff >= b->numrows) {
			w->rowoff = b->numrows - 1;
			w->skip_sublines = 0;
		}
	}

	/* Build screen line cache for each visible buffer (§4.2) */
	for (int i = 0; i < E.nwindows; i++) {
		struct editorBuffer *b = E.windows[i]->buf;
		if (!b->screen_line_cache_valid) {
			buildScreenCache(b, E.screencols);
		}
	}

	int focusedIdx = windowFocusedIdx();

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
			struct editorWindow *win = E.windows[i];
			win->height = window_height;
			if (i == E.nwindows - 1)
				win->height += remaining_height;
		}
	}

	for (int i = 0; i < E.nwindows; i++) {
		struct editorWindow *win = E.windows[i];

		if (win->focused)
			scroll();
		drawRows(win, &ab, win->height, E.screencols);
		updateEndFlag(win, win->buf);
		cumulative_height += win->height + statusbar_height;
		drawStatusBar(win, &ab, cumulative_height);
	}

	drawMinibuffer(&ab);

	// Clear any remaining lines below content
	abAppend(&ab, "\x1b[J", 3);

	// Position the cursor for the focused window
	struct editorWindow *focusedWin = E.windows[focusedIdx];
	char buf[32];

	int cursor_y = focusedWin->scy + 1; // 1-based index
	for (int i = 0; i < focusedIdx; i++) {
		cursor_y += E.windows[i]->height + statusbar_height;
	}

	// Ensure cursor doesn't go beyond the window's bottom
	if (cursor_y > cumulative_height) {
		struct editorBuffer *buf = focusedWin->buf;
		if (buf->cy >= buf->numrows) {
			cursor_y = cumulative_height;
		} else {
			cursor_y = cumulative_height - statusbar_height;
		}
	}

	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cursor_y,
		 focusedWin->scx + 1);
	abAppend(&ab, buf, strlen(buf));
	abAppend(&ab, "\x1b[?7h", 5);  // Enable auto-wrap
	abAppend(&ab, "\x1b[?25h", 6); // Show cursor

	IGNORE_RETURN(write(STDOUT_FILENO, ab.b, ab.len));

	//	usleep(100000); // 100ms delay for simulating slow network or screen
	abFree(&ab);
}

void cursorBottomLine(int curs) {
	char cbuf[32];
	/* Calculate actual minibuffer row position */
	int minibuf_row = 0;
	for (int i = 0; i < E.nwindows; i++) {
		minibuf_row += E.windows[i]->height + statusbar_height;
	}
	minibuf_row++; /* minibuffer starts after all windows/status bars */

	/* For multi-line minibuffer, figure out which line and column
	 * the cursor falls on.  The first line has (screencols - prompt_width)
	 * available characters; continuation lines have screencols each. */
	if (minibuffer_height > 1 && curs > E.screencols) {
		/* curs is 1-based column position.  First line holds screencols
		 * characters.  Each subsequent line holds screencols more. */
		int extra_lines = (curs - 1) / E.screencols;
		if (extra_lines >= minibuffer_height)
			extra_lines = minibuffer_height - 1;
		minibuf_row += extra_lines;
		curs = curs - extra_lines * E.screencols;
	}

	snprintf(cbuf, sizeof(cbuf), CSI "%d;%dH", minibuf_row, curs);
	IGNORE_RETURN(write(STDOUT_FILENO, cbuf, strlen(cbuf)));
}

void editorResizeScreen(int UNUSED(sig)) {
	if (getWindowSize(&E.screenrows, &E.screencols) == -1)
		die("getWindowSize");
	/* Screen width changed — all cached widths are stale for word-wrap */
	for (struct editorBuffer *b = E.headbuf; b != NULL; b = b->next) {
		for (int i = 0; i < b->numrows; i++) {
			b->row[i].cached_width = -1;
		}
		b->screen_line_cache_valid = 0;
	}
	/* Reset window heights so they get recalculated */
	for (int i = 0; i < E.nwindows; i++) {
		E.windows[i]->height = 0;
	}
	computeDisplayNames();
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
	win->skip_sublines = 0;
}

void editorToggleVisualLineMode(void) {
	E.buf->word_wrap = !E.buf->word_wrap;
	invalidateScreenCache(E.buf);
	editorSetStatusMessage(E.buf->word_wrap ? "Visual line mode enabled" :
						  "Visual line mode disabled");
}

void editorVersion(void) {
	editorSetStatusMessage("emil %s" EMIL_VERSION);
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
