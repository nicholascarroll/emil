#include "display.h"
#include "abuf.h"
#include "edit.h"
#include "emil.h"
#include "message.h"
#include "terminal.h"
#include "unicode.h"
#include "unused.h"
#include "region.h"
#include "buffer.h"
#include "util.h"
#include "wcwidth.h"
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

/* Check if a render position is within the marked region */
static int isRenderPosInRegion(struct editorBuffer *buf, int row,
			       int render_pos) {
	if (markInvalidSilent())
		return 0;

	erow *erow_ptr = &buf->row[row];
	if (!erow_ptr)
		return 0;

	if (buf->rectangle_mode) {
		int top_row = buf->cy < buf->marky ? buf->cy : buf->marky;
		int bottom_row = buf->cy > buf->marky ? buf->cy : buf->marky;
		int left_col = buf->cx < buf->markx ? buf->cx : buf->markx;
		int right_col = buf->cx > buf->markx ? buf->cx : buf->markx;

		if (row < top_row || row > bottom_row)
			return 0;

		int left_render = charsToDisplayColumn(erow_ptr, left_col);
		int right_render = charsToDisplayColumn(erow_ptr, right_col);

		return (render_pos >= left_render && render_pos < right_render);
	} else {
		int start_row = buf->cy < buf->marky ? buf->cy : buf->marky;
		int end_row = buf->cy > buf->marky ? buf->cy : buf->marky;
		int start_col =
			(buf->cy < buf->marky ||
			 (buf->cy == buf->marky && buf->cx <= buf->markx)) ?
				buf->cx :
				buf->markx;
		int end_col =
			(buf->cy > buf->marky ||
			 (buf->cy == buf->marky && buf->cx >= buf->markx)) ?
				buf->cx :
				buf->markx;

		if (row < start_row || row > end_row)
			return 0;

		if (row == start_row && row == end_row) {
			int start_render =
				charsToDisplayColumn(erow_ptr, start_col);
			int end_render =
				charsToDisplayColumn(erow_ptr, end_col);
			return (render_pos >= start_render &&
				render_pos < end_render);
		}
		if (row == start_row) {
			int start_render =
				charsToDisplayColumn(erow_ptr, start_col);
			return (render_pos >= start_render);
		}
		if (row == end_row) {
			int end_render =
				charsToDisplayColumn(erow_ptr, end_col);
			return (render_pos < end_render);
		}
		return 1; /* Entire middle row is in region */
	}
}

/* Check if a render position is at the current search match */
static int isRenderPosCurrentSearchMatch(struct editorBuffer *buf, int row,
					 int render_pos) {
	if (!buf->query || !buf->query[0] || !buf->match)
		return 0;
	if (row != buf->cy)
		return 0;

	erow *erow_ptr = &buf->row[row];
	if (!erow_ptr)
		return 0;

	int match_len = strlen((char *)buf->query);
	int start_render = charsToDisplayColumn(erow_ptr, buf->cx);
	int end_render = charsToDisplayColumn(erow_ptr, buf->cx + match_len);

	return (render_pos >= start_render && render_pos < end_render);
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

/* Render a line with highlighting support */
static void renderLineWithHighlighting(erow *row, struct abuf *ab,
				       int start_col, int end_col,
				       struct editorBuffer *buf, int filerow) {
	int render_x = 0;
	int char_idx = 0;
	int current_highlight = 0;

	/* Skip to start column */
	while (char_idx < row->size && render_x < start_col) {
		if (row->chars[char_idx] < 0x80 &&
		    !ISCTRL(row->chars[char_idx])) {
			render_x += 1;
			char_idx++;
		} else {
			render_x = nextScreenX(row->chars, &char_idx, render_x);
			char_idx++;
		}
	}

	/* Render visible portion */
	while (char_idx < row->size && render_x < end_col) {
		uint8_t c = row->chars[char_idx];

		int in_region = isRenderPosInRegion(buf, filerow, render_x);
		int is_current_match =
			isRenderPosCurrentSearchMatch(buf, filerow, render_x);
		updateHighlight(ab, &current_highlight,
				(in_region || is_current_match) ? 1 : 0);

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
				renderLineWithHighlighting(
					row, ab, win->coloff,
					win->coloff + screencols, buf, filerow);
				filerow++;
			} else {
				/* Word-wrap mode: break lines at word
				 * boundaries when possible. */
				int line_start_col = 0;
				int line_start_byte = 0;

				while (line_start_byte < row->size &&
				       y < screenrows) {
					/* --- Find the break point for this
					 *     screen line --- */
					int col = line_start_col;
					int bidx = line_start_byte;
					/* Last word-boundary position seen */
					int wb_col = -1;
					int wb_byte = -1;

					while (bidx < row->size) {
						uint8_t c = row->chars[bidx];
						int cwidth;

						if (c == '\t') {
							cwidth =
								EMIL_TAB_STOP -
								(col %
								 EMIL_TAB_STOP);
						} else if (ISCTRL(c)) {
							cwidth = 2;
						} else {
							cwidth = charInStringWidth(
								row->chars,
								bidx);
						}

						/* Wide char won't fit: leave
						 * a 1-col gap and break. */
						if (cwidth > 1 &&
						    col + cwidth - line_start_col >
							    screencols) {
							break;
						}

						if (col + cwidth -
							    line_start_col >
						    screencols) {
							break;
						}

						/* Track word boundaries:
						 * the break point is
						 * *after* the boundary
						 * character. */
						if (isWordBoundary(c)) {
							int nbytes =
								utf8_nBytes(c);
							wb_col = col + cwidth;
							wb_byte = bidx + nbytes;
						}

						col += cwidth;
						bidx += utf8_nBytes(c);
					}

					int break_col, break_byte;
					if (bidx >= row->size) {
						/* Rest of row fits on this
						 * screen line. */
						break_col = col;
						break_byte = row->size;
					} else if (wb_col > line_start_col) {
						/* Break at the last word
						 * boundary we found. */
						break_col = wb_col;
						break_byte = wb_byte;
					} else {
						/* No word boundary found —
						 * hard break at column
						 * limit. */
						break_col = col;
						break_byte = bidx;
					}

					/* --- Render the span --- */
					renderLineWithHighlighting(
						row, ab, line_start_col,
						break_col, buf, filerow);

					/* --- Fill trailing space with
					 *     correct highlighting --- */
					int fill_col = break_col;
					int hl = 0;
					while (fill_col - line_start_col <
					       screencols) {
						int in_r = isRenderPosInRegion(
							buf, filerow, fill_col);
						int in_m =
							isRenderPosCurrentSearchMatch(
								buf, filerow,
								fill_col);
						updateHighlight(
							ab, &hl,
							(in_r || in_m) ? 1 : 0);
						abAppend(ab, " ", 1);
						fill_col++;
					}
					updateHighlight(ab, &hl, 0);

					/* --- Advance to next screen line
					 *     if more content remains --- */
					if (break_byte < row->size &&
					    y < screenrows - 1) {
						abAppend(ab, "\r\n", 2);
						y++;
					}

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

	char perc[8] = " xxx --";
	if (bufr->numrows == 0) {
		perc[1] = 'E';
		perc[2] = 'm';
		perc[3] = 'p';
	} else if (bufr->end) {
		if (win->rowoff == 0) {
			perc[1] = 'A';
			perc[2] = 'l';
			perc[3] = 'l';
		} else {
			perc[1] = 'B';
			perc[2] = 'o';
			perc[3] = 't';
		}
	} else if (win->rowoff == 0) {
		perc[1] = 'T';
		perc[2] = 'o';
		perc[3] = 'p';
	} else {
		snprintf(perc, sizeof(perc), " %2d%% --",
			 (win->rowoff * 100) / bufr->numrows);
	}

	char fill[2] = "-";
	if (!win->focused) {
		perc[5] = ' ';
		perc[6] = ' ';
		fill[0] = ' ';
	}

	if (len > E.screencols)
		len = E.screencols;
	abAppend(ab, status, len);
	while (len < E.screencols) {
		if (E.screencols - len == 7) {
			abAppend(ab, perc, 7);
			break;
		} else {
			abAppend(ab, fill, 1);
			len++;
		}
	}
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
	struct abuf ab = ABUF_INIT;
	abAppend(&ab, "\x1b[?25l", 6); // Hide cursor
	/* possible flicker reduction on local terminals
But might be adding a packet in SSH
write(STDOUT_FILENO, ab.b, ab.len);
ab.len = 0;
*/
	abAppend(&ab, "\x1b[H", 3);    // Move cursor to top-left corner





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
	/* Calculate rendered column position (accounting for tabs and Unicode) */
	int rx = 0;
	int line_len = 0;
	if (E.buf->cy < E.buf->numrows) {
		struct erow *row = &E.buf->row[E.buf->cy];
		line_len = row->size;
		for (int j = 0; j < E.buf->cx && j < row->size; j++) {
			if (row->chars[j] == '\t') {
				rx = (rx + EMIL_TAB_STOP) &
				     ~(EMIL_TAB_STOP - 1);
			} else {
				int w = mk_wcwidth(row->chars[j]);
				rx += (w > 0) ? w : 1;
			}
		}
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
