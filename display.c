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

/* Compute the terminal row for the cursor in the focused window. */
static int computeCursorY(struct editorWindow *focusedWin, int focusedIdx) {
	int cursor_y = focusedWin->scy + 1; /* 1-based */
	for (int i = 0; i < focusedIdx; i++)
		cursor_y += E.windows[i]->height + statusbar_height;

	int cumulative = 0;
	for (int i = 0; i < E.nwindows; i++)
		cumulative += E.windows[i]->height + statusbar_height;

	if (cursor_y > cumulative) {
		struct editorBuffer *buf = focusedWin->buf;
		cursor_y = (buf->cy >= buf->numrows) ?
				   cumulative :
				   cumulative - statusbar_height;
	}
	return cursor_y;
}

void refreshScreen(void) {
	int focusedIdx = windowFocusedIdx();
	struct editorWindow *focusedWin = E.windows[focusedIdx];

	/* Always run scroll to keep cursor on screen and update scx/scy */
	int prev_rowoff = focusedWin->rowoff;
	int prev_coloff = focusedWin->coloff;
	scroll();

	/* If scroll changed the viewport, upgrade hint to full redraw */
	if (focusedWin->rowoff != prev_rowoff ||
	    focusedWin->coloff != prev_coloff) {
		E.hint.type = REFRESH_FULL;
	}

	enum refreshType hint_type = E.hint.type;
	E.hint.type = REFRESH_FULL; /* Reset to safe default */

	/* --- Cursor-only fast path --- */
	if (hint_type == REFRESH_CURSOR_ONLY) {
		struct abuf ab = ABUF_INIT;

		/* Redraw status bar (shows line:col) and minibuffer
		 * (shows status messages) so they stay current. */
		int status_row = 0;
		for (int i = 0; i <= focusedIdx; i++)
			status_row += E.windows[i]->height + statusbar_height;
		drawStatusBar(focusedWin, &ab, status_row);

		/* Redraw minibuffer for status messages */
		int minibuf_row = 0;
		for (int i = 0; i < E.nwindows; i++)
			minibuf_row += E.windows[i]->height + statusbar_height;
		minibuf_row++;
		char mbuf[32];
		snprintf(mbuf, sizeof(mbuf), CSI "%d;1H", minibuf_row);
		abAppend(&ab, mbuf, strlen(mbuf));
		drawMinibuffer(&ab);

		/* Position cursor */
		char buf[32];
		int cursor_y = computeCursorY(focusedWin, focusedIdx);
		snprintf(buf, sizeof(buf), CSI "%d;%dH", cursor_y,
			 focusedWin->scx + 1);
		abAppend(&ab, buf, strlen(buf));

		write(STDOUT_FILENO, ab.b, ab.len);
		abFree(&ab);
		return;
	}

	/* --- Full redraw path (existing behavior) --- */
	struct abuf ab = ABUF_INIT;
	abAppend(&ab, "\x1b[?25l", 6); // Hide cursor
	abAppend(&ab, "\x1b[H", 3);    // Move cursor to top-left corner

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

		/* scroll() was already called above for the focused window */
		drawRows(win, &ab, win->height, E.screencols);
		cumulative_height += win->height + statusbar_height;
		drawStatusBar(win, &ab, cumulative_height);
	}

	drawMinibuffer(&ab);

	// Clear any remaining lines below content
	abAppend(&ab, "\x1b[J", 3);

	// Position the cursor for the focused window
	char buf[32];
	int cursor_y = computeCursorY(focusedWin, focusedIdx);

	snprintf(buf, sizeof(buf), "\x1b[%d;%dH", cursor_y,
		 focusedWin->scx + 1);
	abAppend(&ab, buf, strlen(buf));

	abAppend(&ab, "\x1b[?25h", 6); // Show cursor

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
	refreshHint(REFRESH_CURSOR_ONLY);
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
