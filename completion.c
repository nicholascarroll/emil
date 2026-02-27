#include <glob.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdio.h>
#include "emil.h"
#include "message.h"
#include "completion.h"
#include "buffer.h"
#include "util.h"
#include "display.h"
#include "terminal.h"
#include "fileio.h"
#include "prompt.h"
#include "edit.h"
#include "unicode.h"
#include "undo.h"
#include <regex.h>

extern struct editorConfig E;

void resetCompletionState(struct completion_state *state) {
	free(state->last_completed_text);
	state->last_completed_text = NULL;
	state->completion_start_pos = 0;
	state->successive_tabs = 0;
	state->last_completion_count = 0;
	state->preserve_message = 0;
	if (state->matches) {
		for (int i = 0; i < state->n_matches; i++)
			free(state->matches[i]);
		free(state->matches);
	}
	state->matches = NULL;
	state->n_matches = 0;
	state->selected = -1;
}

static void freeCompletionResult(struct completion_result *result) {
	if (result->matches) {
		for (int i = 0; i < result->n_matches; i++) {
			free(result->matches[i]);
		}
		free(result->matches);
	}
	free(result->common_prefix);
	result->matches = NULL;
	result->common_prefix = NULL;
	result->n_matches = 0;
	result->prefix_len = 0;
}

static char *findCommonPrefix(char **strings, int count) {
	if (count == 0)
		return NULL;
	if (count == 1)
		return xstrdup(strings[0]);

	int prefix_len = 0;
	while (1) {
		char ch = strings[0][prefix_len];
		if (ch == '\0')
			break;

		int all_match = 1;
		for (int i = 1; i < count; i++) {
			if (strings[i][prefix_len] != ch) {
				all_match = 0;
				break;
			}
		}

		if (!all_match)
			break;
		prefix_len++;
	}

	char *prefix = xmalloc(prefix_len + 1);
	emil_strlcpy(prefix, strings[0], prefix_len + 1);
	return prefix;
}

static void getFileCompletions(const char *prefix,
			       struct completion_result *result) {
	glob_t globlist;
	result->matches = NULL;
	result->n_matches = 0;
	result->common_prefix = NULL;
	result->prefix_len = strlen(prefix);

	char *glob_pattern = NULL;
	const char *pattern_to_use = prefix;

	/* Manual tilde expansion */
	if (*prefix == '~') {
		char *home_dir = getenv("HOME");
		if (!home_dir) {
			return;
		}

		size_t home_len = strlen(home_dir);
		size_t prefix_len = strlen(prefix);
		char *expanded = xmalloc(home_len + prefix_len);
		emil_strlcpy(expanded, home_dir, home_len + prefix_len);
		emil_strlcat(expanded, prefix + 1, home_len + prefix_len);
		pattern_to_use = expanded;
	}

#ifndef EMIL_NO_SIMPLE_GLOB
	/* Add * for globbing */
	int len = strlen(pattern_to_use);
	glob_pattern = xmalloc(len + 2);
	emil_strlcpy(glob_pattern, pattern_to_use, len + 2);
	glob_pattern[len] = '*';
	glob_pattern[len + 1] = '\0';

	if (pattern_to_use != prefix) {
		free((void *)pattern_to_use);
	}
	pattern_to_use = glob_pattern;
#endif

	int glob_result = glob(pattern_to_use, GLOB_MARK, NULL, &globlist);
	if (glob_result == 0) {
		if (globlist.gl_pathc > 0) {
			result->matches =
				xmalloc(globlist.gl_pathc * sizeof(char *));
			result->n_matches = globlist.gl_pathc;

			for (size_t i = 0; i < globlist.gl_pathc; i++) {
				result->matches[i] =
					xstrdup(globlist.gl_pathv[i]);
			}

			result->common_prefix = findCommonPrefix(
				result->matches, result->n_matches);
		}
		globfree(&globlist);
	} else if (glob_result == GLOB_NOMATCH) {
		/* No matches found */
		result->n_matches = 0;
	}

	if (glob_pattern) {
		free(glob_pattern);
	}
	if (pattern_to_use != prefix && pattern_to_use != glob_pattern) {
		free((void *)pattern_to_use);
	}
}

static void getBufferCompletions(struct editorConfig *ed, const char *prefix,
				 struct editorBuffer *currentBuffer,
				 struct completion_result *result) {
	result->matches = NULL;
	result->n_matches = 0;
	result->common_prefix = NULL;
	result->prefix_len = strlen(prefix);

	int capacity = 8;
	result->matches = xmalloc(capacity * sizeof(char *));

	/* We also collect basenames for computing the common prefix,
	 * since the user types basenames in the prompt. */
	char **basenames = xmalloc(capacity * sizeof(char *));

	for (struct editorBuffer *b = ed->headbuf; b != NULL; b = b->next) {
		if (b == currentBuffer)
			continue;

		/* Skip the *Completions* buffer */
		if (b->filename && strcmp(b->filename, "*Completions*") == 0)
			continue;

		char *name = b->filename ? b->filename : "*scratch*";

		/* Match against the basename portion */
		const char *slash = strrchr(name, '/');
		const char *base = slash ? slash + 1 : name;

		if (strncmp(base, prefix, strlen(prefix)) == 0) {
			if (result->n_matches >= capacity) {
				if (capacity > INT_MAX / 2 ||
				    (size_t)capacity >
					    SIZE_MAX / (2 * sizeof(char *))) {
					die("buffer size overflow");
				}
				capacity *= 2;
				result->matches =
					xrealloc(result->matches,
						 capacity * sizeof(char *));
				basenames = xrealloc(basenames,
						     capacity * sizeof(char *));
			}
			result->matches[result->n_matches] = xstrdup(name);
			basenames[result->n_matches] = xstrdup(base);
			result->n_matches++;
		}
	}

	if (result->n_matches > 0) {
		/* Compute common prefix over basenames so TAB-completion
		 * extends the basename the user is typing. */
		result->common_prefix =
			findCommonPrefix(basenames, result->n_matches);
	} else {
		free(result->matches);
		result->matches = NULL;
	}

	for (int i = 0; i < result->n_matches; i++)
		free(basenames[i]);
	free(basenames);
}

static void getCommandCompletions(struct editorConfig *ed, const char *prefix,
				  struct completion_result *result) {
	result->matches = NULL;
	result->n_matches = 0;
	result->common_prefix = NULL;
	result->prefix_len = strlen(prefix);

	int capacity = 8;
	result->matches = xmalloc(capacity * sizeof(char *));

	/* Convert prefix to lowercase for case-insensitive matching */
	int prefix_len = strlen(prefix);
	char *lower_prefix = xmalloc(prefix_len + 1);
	for (int i = 0; i <= prefix_len; i++) {
		char c = prefix[i];
		if ('A' <= c && c <= 'Z') {
			c |= 0x60;
		}
		lower_prefix[i] = c;
	}

	for (int i = 0; i < ed->cmd_count; i++) {
		if (strncmp(ed->cmd[i].key, lower_prefix, prefix_len) == 0) {
			if (result->n_matches >= capacity) {
				if (capacity > INT_MAX / 2 ||
				    (size_t)capacity >
					    SIZE_MAX / (2 * sizeof(char *))) {
					die("buffer size overflow");
				}
				capacity *= 2;
				result->matches =
					xrealloc(result->matches,
						 capacity * sizeof(char *));
			}
			result->matches[result->n_matches++] =
				xstrdup(ed->cmd[i].key);
		}
	}

	free(lower_prefix);

	if (result->n_matches > 0) {
		result->common_prefix =
			findCommonPrefix(result->matches, result->n_matches);
	} else {
		free(result->matches);
		result->matches = NULL;
	}
}

static void replaceMinibufferText(struct editorBuffer *minibuf,
				  const char *text) {
	/* Clear current content */
	while (minibuf->numrows > 0) {
		editorDelRow(minibuf, 0);
	}

	/* Insert new text */
	editorInsertRow(minibuf, 0, (char *)text, strlen(text));
	minibuf->cx = strlen(text);
	minibuf->cy = 0;
}

static struct editorBuffer *findOrCreateBuffer(const char *name) {
	/* Search for existing buffer */
	for (struct editorBuffer *b = E.headbuf; b != NULL; b = b->next) {
		if (b->filename && strcmp(b->filename, name) == 0) {
			return b;
		}
	}

	/* Create new buffer */
	struct editorBuffer *new_buf = newBuffer();
	new_buf->filename = xstrdup(name);
	new_buf->special_buffer = 1;
	new_buf->next = E.headbuf;
	E.headbuf = new_buf;
	return new_buf;
}

static void clearBuffer(struct editorBuffer *buf) {
	while (buf->numrows > 0) {
		editorDelRow(buf, 0);
	}
}

static void showCompletionsBuffer(char **matches, int n_matches,
				  enum promptType type) {
	/* Find or create completions buffer */
	struct editorBuffer *comp_buf = findOrCreateBuffer("*Completions*");
	clearBuffer(comp_buf);
	comp_buf->read_only = 1;
	comp_buf->word_wrap = 0;

	/* Add header */
	char header[100];
	snprintf(header, sizeof(header),
		 "Possible completions (%d):", n_matches);
	editorInsertRow(comp_buf, 0, header, strlen(header));
	editorInsertRow(comp_buf, 1, "", 0);

	if (type == PROMPT_BASIC) {
		/* Buffer completions: vertical list with display names.
		 * Show one match per row using display_name for each
		 * buffer.  The basename is highlighted by the renderer. */
		for (int i = 0; i < n_matches; i++) {
			/* Find the buffer to get its display_name */
			const char *show = matches[i];
			for (struct editorBuffer *b = E.headbuf; b != NULL;
			     b = b->next) {
				const char *bname = b->filename ? b->filename :
								  "*scratch*";
				if (strcmp(bname, matches[i]) == 0) {
					if (b->display_name)
						show = b->display_name;
					break;
				}
			}
			int len = (int)strlen(show);
			editorInsertRow(comp_buf, comp_buf->numrows,
					(char *)show, len);
		}

		/* Store match list for M-n/M-p navigation. */
		struct completion_state *cs = &E.minibuf->completion_state;
		if (cs->matches) {
			for (int i = 0; i < cs->n_matches; i++)
				free(cs->matches[i]);
			free(cs->matches);
		}
		cs->matches = xmalloc(n_matches * sizeof(char *));
		cs->n_matches = n_matches;
		for (int i = 0; i < n_matches; i++)
			cs->matches[i] = xstrdup(matches[i]);
		cs->selected = 0;

		/* Track selected row for highlighting (data starts row 2). */
		comp_buf->cy = 2;
	} else {
		/* File/command completions: columnar layout. */
		int max_width = 0;
		for (int i = 0; i < n_matches; i++) {
			int width = stringWidth((uint8_t *)matches[i]);
			if (width > max_width)
				max_width = width;
		}

		int term_width = E.screencols;
		int col_width = max_width + 2;
		int columns = term_width / col_width;
		if (columns < 1)
			columns = 1;

		int rows = (n_matches + columns - 1) / columns;
		for (int row = 0; row < rows; row++) {
			char line[1024] = { 0 };
			int line_pos = 0;

			for (int col = 0; col < columns; col++) {
				int idx = row + col * rows;
				if (idx >= n_matches)
					break;

				int written = snprintf(line + line_pos,
						       sizeof(line) - line_pos,
						       "%-*s", col_width,
						       matches[idx]);
				if (written > 0)
					line_pos += written;
			}

			while (line_pos > 0 && line[line_pos - 1] == ' ')
				line_pos--;
			line[line_pos] = '\0';

			editorInsertRow(comp_buf, comp_buf->numrows, line,
					line_pos);
		}
	}

	/* Display in window if not already visible */
	int comp_window = findBufferWindow(comp_buf);
	if (comp_window == -1) {
		/* Not visible - create new window at bottom */
		if (E.nwindows >= 1) {
			/* Create new window for completions */
			int new_window_idx = E.nwindows;
			editorCreateWindow();

			/* Set the new window to show completions buffer */
			E.windows[new_window_idx]->buf = comp_buf;
			E.windows[new_window_idx]->focused = 0;

			/* Keep focus on the first window */
			for (int i = 0; i < E.nwindows; i++) {
				E.windows[i]->focused = (i == 0);
			}

			comp_window = new_window_idx;
		}
	} else {
	}

	/* Adjust window sizes for completions display */
	if (E.nwindows >= 2 && comp_window >= 0) {
		/* Calculate desired height for completions window */
		int comp_height = comp_buf->numrows + 2; /* +2 for padding */

		/* Calculate total available height */
		int total_height = E.screenrows - minibuffer_height -
				   (statusbar_height * E.nwindows);

		/* Calculate minimum space needed for non-completion windows */
		int non_comp_windows = E.nwindows - 1;
		int min_space_for_others =
			non_comp_windows * 3; /* 3 lines minimum each */

		/* Maximum height for completions is what's left after ensuring minimums */
		int max_comp_height = total_height - min_space_for_others;
		if (comp_height > max_comp_height) {
			comp_height = max_comp_height;
		}

		/* Ensure completions window itself gets at least 3 lines */
		if (comp_height < 3) {
			comp_height = 3;
		}

		/* Distribute remaining space to other windows */
		int remaining_height = total_height - comp_height;
		int height_per_window = remaining_height / non_comp_windows;

		/* Set window heights */
		for (int i = 0; i < E.nwindows; i++) {
			if (i == comp_window) {
				E.windows[i]->height = comp_height;
			} else {
				E.windows[i]->height = height_per_window;
			}
		}
	}

	refreshScreen();
}

void closeCompletionsBuffer(void) {
	struct editorBuffer *comp_buf = NULL;
	struct editorBuffer *prev_buf = NULL;

	/* Find the completions buffer and its predecessor */
	for (struct editorBuffer *b = E.headbuf; b != NULL;
	     prev_buf = b, b = b->next) {
		if (b->filename && strcmp(b->filename, "*Completions*") == 0) {
			comp_buf = b;
			break;
		}
	}

	if (comp_buf) {
		int comp_window = findBufferWindow(comp_buf);
		if (comp_window >= 0 && E.nwindows > 1) {
			editorDestroyWindow(comp_window);
		}

		/* Remove the buffer from the buffer list */
		if (prev_buf) {
			prev_buf->next = comp_buf->next;
		} else {
			E.headbuf = comp_buf->next;
		}

		/* Update E.buf if it pointed to the completions buffer */
		if (E.buf == comp_buf) {
			E.buf = comp_buf->next ? comp_buf->next : E.headbuf;
		}

		/* Update lastVisitedBuffer if it pointed to completions buffer */
		if (E.lastVisitedBuffer == comp_buf) {
			E.lastVisitedBuffer = NULL;
		}

		/* Destroy the buffer */
		destroyBuffer(comp_buf);
	}
}

void handleMinibufferCompletion(struct editorBuffer *minibuf,
				enum promptType type) {
	/* Get current buffer text */
	char *current_text =
		minibuf->numrows > 0 ? (char *)minibuf->row[0].chars : "";

	/* Check if text changed since last completion */
	if (minibuf->completion_state.last_completed_text == NULL ||
	    strcmp(current_text,
		   minibuf->completion_state.last_completed_text) != 0) {
		/* Text changed - reset completion state */
		resetCompletionState(&minibuf->completion_state);
	}

	/* Get matches based on type */
	struct completion_result result;
	switch (type) {
	case PROMPT_FILES:
		getFileCompletions(current_text, &result);
		break;
	case PROMPT_DIR:
		getFileCompletions(current_text, &result);
		/* Filter to directories only (trailing '/') */
		if (result.n_matches > 0) {
			int dst = 0;
			for (int i = 0; i < result.n_matches; i++) {
				int len = (int)strlen(result.matches[i]);
				if (len > 0 &&
				    result.matches[i][len - 1] == '/') {
					if (dst != i) {
						free(result.matches[dst]);
						result.matches[dst] =
							result.matches[i];
						result.matches[i] = NULL;
					}
					dst++;
				} else {
					free(result.matches[i]);
					result.matches[i] = NULL;
				}
			}
			result.n_matches = dst;
			if (result.n_matches > 0) {
				free(result.common_prefix);
				result.common_prefix = findCommonPrefix(
					result.matches, result.n_matches);
			} else {
				free(result.common_prefix);
				result.common_prefix = NULL;
				free(result.matches);
				result.matches = NULL;
			}
		}
		break;
	case PROMPT_BASIC:
		getBufferCompletions(&E, current_text, E.edbuf, &result);
		break;
	case PROMPT_COMMAND:
		getCommandCompletions(&E, current_text, &result);
		break;
	case PROMPT_SEARCH:
		/* For search, we can provide buffer completions */
		getBufferCompletions(&E, current_text, E.edbuf, &result);
		break;
	}

	/* Handle based on number of matches */
	if (result.n_matches == 0) {
		editorSetStatusMessage(msg_no_match_bracket);
		minibuf->completion_state.preserve_message = 1;
	} else if (result.n_matches == 1) {
		/* Complete fully */
		replaceMinibufferText(minibuf, result.matches[0]);
		closeCompletionsBuffer();
	} else {
		/* Multiple matches */
		if (result.common_prefix &&
		    strlen(result.common_prefix) > strlen(current_text)) {
			/* Can extend to common prefix */
			replaceMinibufferText(minibuf, result.common_prefix);
			closeCompletionsBuffer();
		} else {
			/* Already at common prefix (or no common prefix found) */
			if (minibuf->completion_state.successive_tabs > 0) {
				showCompletionsBuffer(result.matches,
						      result.n_matches, type);
			} else {
				editorSetStatusMessage(
					"[Complete, but not unique]");
				minibuf->completion_state.preserve_message = 1;
			}
		}
	}

	/* Update state BEFORE cleanup */
	minibuf->completion_state.successive_tabs++;
	free(minibuf->completion_state.last_completed_text);
	minibuf->completion_state.last_completed_text = xstrdup(
		minibuf->numrows > 0 ? (char *)minibuf->row[0].chars : "");

	/* Cleanup */
	freeCompletionResult(&result);
}

void cycleCompletion(struct editorBuffer *minibuf, int direction) {
	struct completion_state *cs = &minibuf->completion_state;
	if (!cs->matches || cs->n_matches == 0)
		return;

	/* Cycle selection */
	cs->selected += direction;
	if (cs->selected >= cs->n_matches)
		cs->selected = 0;
	if (cs->selected < 0)
		cs->selected = cs->n_matches - 1;

	/* Update minibuffer text to the basename of the selected match */
	const char *match = cs->matches[cs->selected];
	const char *slash = strrchr(match, '/');
	const char *base = slash ? slash + 1 : match;
	replaceMinibufferText(minibuf, base);

	/* Update last_completed_text so TAB doesn't reset */
	free(cs->last_completed_text);
	cs->last_completed_text = xstrdup(base);

	/* Update the completions buffer cursor to highlight the
	 * selected row.  Data rows start at row 2. */
	for (struct editorBuffer *b = E.headbuf; b != NULL; b = b->next) {
		if (b->filename && strcmp(b->filename, "*Completions*") == 0) {
			b->cy = cs->selected + 2;
			break;
		}
	}
}
