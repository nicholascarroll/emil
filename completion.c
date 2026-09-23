/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include <dirent.h>
#include <glob.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "emil.h"

#include "completion.h"
#include "buffer.h"
#include "dbuf.h"
#include "mutate.h"
#include "util.h"
#include "display.h"
#include "terminal.h"
#include "fileio.h"
#include "prompt.h"
#include "edit.h"
#include "unicode.h"
#include "undo.h"
#include "window.h"
#include <regex.h>

void resetCompletionState(struct completionState *state) {
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

static void freeCompletionResult(struct completionResult *result) {
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
			       struct completionResult *result) {
	glob_t globlist;
	result->matches = NULL;
	result->n_matches = 0;
	result->common_prefix = NULL;
	result->prefix_len = strlen(prefix);

	/* pattern_to_use borrows prefix or points at expanded, the only
	 * one to free. */
	const char *pattern_to_use = prefix;
	char *expanded = NULL;

	/* Manual tilde expansion */
	if (*prefix == '~') {
		char *home_dir = getenv("HOME");
		if (!home_dir) {
			return;
		}

		size_t home_len = strlen(home_dir);
		size_t prefix_len = strlen(prefix);
		expanded = xmalloc(home_len + prefix_len);
		emil_strlcpy(expanded, home_dir, home_len + prefix_len);
		emil_strlcat(expanded, prefix + 1, home_len + prefix_len);
		pattern_to_use = expanded;
	}

	/* Append '*' so the prefix matches as a prefix. */
	int len = strlen(pattern_to_use);
	char *glob_pattern = xmalloc(len + 2);
	emil_strlcpy(glob_pattern, pattern_to_use, len + 2);
	glob_pattern[len] = '*';
	glob_pattern[len + 1] = '\0';

	free(expanded);
	pattern_to_use = glob_pattern;

	int glob_result = glob(pattern_to_use, GLOB_MARK, NULL, &globlist);
	if (glob_result == 0) {
		if (globlist.gl_pathc > 0) {
			result->matches =
				xmalloc(globlist.gl_pathc * sizeof(char *));
			result->n_matches = globlist.gl_pathc;

			for (size_t i = 0; i < globlist.gl_pathc; i++) {
				if (*prefix == '~')
					result->matches[i] = collapseHome(
						globlist.gl_pathv[i]);
				else
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

	free(glob_pattern);
}

static void getBufferCompletions(const char *prefix,
				 struct buffer *currentBuffer,
				 struct completionResult *result) {
	result->matches = NULL;
	result->n_matches = 0;
	result->common_prefix = NULL;
	result->prefix_len = strlen(prefix);

	int capacity = 8;
	result->matches = xmalloc(capacity * sizeof(char *));

	/* We also collect basenames for computing the common prefix,
	 * since the user types basenames in the prompt. */
	char **basenames = xmalloc(capacity * sizeof(char *));

	for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
		if (b == currentBuffer)
			continue;

		/* Skip the *Completions* buffer */
		if (b->filename && strcmp(b->filename, "*Completions*") == 0)
			continue;

		const char *name = b->filename ? b->filename : "*scratch*";

		/* Match against the basename portion */
		const char *slash = strrchr(name, '/');
		const char *base = slash ? slash + 1 : name;

		if (strncmp(base, prefix, strlen(prefix)) == 0) {
			if (result->n_matches >= capacity) {
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

static void getCommandCompletions(const char *prefix,
				  struct completionResult *result) {
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

	for (int i = 0; i < E.cmd_count; i++) {
		if (strncmp(E.cmd[i].key, lower_prefix, prefix_len) == 0) {
			if (result->n_matches >= capacity) {
				capacity *= 2;
				result->matches =
					xrealloc(result->matches,
						 capacity * sizeof(char *));
			}
			result->matches[result->n_matches++] =
				xstrdup(E.cmd[i].key);
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

void replaceMinibufferText(struct buffer *minibuf, const char *text) {
	/* Rebuilding the rows here bypasses the mutation layer, so any
	 * undo record describes text that no longer exists.  Each prompt
	 * session gets its own undo history. */
	clearUndosAndRedos(minibuf);

	/* Clear current content */
	while (minibuf->numrows > 0) {
		delRow(minibuf, 0);
	}

	/* Split on newlines rather than storing the byte: a history
	 * entry can contain one (C-q C-j in a replace pattern).*/
	int at = 0;
	const char *p = text;
	for (;;) {
		const char *nl = strchr(p, '\n');
		size_t len = nl ? (size_t)(nl - p) : strlen(p);
		insertRow(minibuf, at, (const uint8_t *)p, len);
		minibuf->cx = (int)len;
		minibuf->cy = at;
		at++;
		if (!nl)
			break;
		p = nl + 1;
	}
}

static void showCompletionsBuffer(char **matches, int n_matches,
				  enum promptType type) {
	/* Find or create completions buffer */
	struct buffer *comp_buf = findOrCreateSpecialBuffer("*Completions*");
	bufferResetRows(comp_buf);
	comp_buf->read_only = 1;
	comp_buf->word_wrap = 0;

	/* Add header */
	char header[100];
	snprintf(header, sizeof(header),
		 "Possible completions (%d):", n_matches);
	insertRow(comp_buf, 0, (const uint8_t *)header, strlen(header));
	insertRow(comp_buf, 1, (const uint8_t *)"", 0);

	if (type == PROMPT_BUFFER) {
		/* Buffer completions: vertical list with display names.
		 * Show one match per row using display_name for each
		 * buffer.  The basename is highlighted by the renderer. */
		for (int i = 0; i < n_matches; i++) {
			/* Find the buffer to get its display_name */
			const char *show = matches[i];
			for (struct buffer *b = E.headbuf; b != NULL;
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
			insertRow(comp_buf, comp_buf->numrows,
				  (const uint8_t *)show, len);
		}

		/* Store match list for M-n/M-p navigation. */
		struct completionState *cs = &E.minibuf->completionState;
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
		/* File/command completions: columnar layout.
		 * Left-truncate long names so the basename is always
		 * visible (issue #31). */
		int term_width = E.screencols;

		/* Build truncated copies for display. */
		char **display = xmalloc(n_matches * sizeof(char *));
		for (int i = 0; i < n_matches; i++)
			display[i] = leftTruncate(matches[i], term_width - 2);

		int max_width = 0;
		for (int i = 0; i < n_matches; i++) {
			int width = stringWidth((uint8_t *)display[i]);
			if (width > max_width)
				max_width = width;
		}

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

				/* Pad by display COLUMNS, not bytes
				 * (DEF-1/#117).  "%-*s" counts bytes, so
				 * an 18-byte 6-column CJK name got no
				 * padding at all from "%-8s" and occupied
				 * 6 columns where the grid expected 8,
				 * misaligning every column after the
				 * first.  col_width came from stringWidth
				 * a few lines up, so the padding must be
				 * measured the same way. */
				int written = snprintf(line + line_pos,
						       sizeof(line) - line_pos,
						       "%s", display[idx]);
				if (written < 0)
					break;
				if (written >= (int)(sizeof(line) - line_pos)) {
					/* Truncated: the row is full. */
					line_pos = sizeof(line) - 1;
					break;
				}
				line_pos += written;

				int pad = col_width -
					  stringWidth((uint8_t *)display[idx]);
				while (pad > 0 &&
				       line_pos < (int)sizeof(line) - 1) {
					line[line_pos++] = ' ';
					pad--;
				}
			}

			while (line_pos > 0 && line[line_pos - 1] == ' ')
				line_pos--;
			line[line_pos] = '\0';

			insertRow(comp_buf, comp_buf->numrows,
				  (const uint8_t *)line, line_pos);
		}

		for (int i = 0; i < n_matches; i++)
			free(display[i]);
		free(display);
	}

	showPopupBuffer(comp_buf);
	refreshScreen();
}

void closeCompletionsBuffer(void) {
	closeSpecialBuffer("*Completions*");
}

void handleMinibufferCompletion(struct buffer *minibuf, enum promptType type) {
	/* Get current buffer text */
	char *current_text = (char *)minibuf->row[0].chars;

	/* Check if text changed since last completion */
	if (minibuf->completionState.last_completed_text == NULL ||
	    strcmp(current_text,
		   minibuf->completionState.last_completed_text) != 0) {
		/* Text changed - reset completion state */
		resetCompletionState(&minibuf->completionState);
	}

	/* Get matches based on type */
	struct completionResult result = { 0 };
	switch (type) {
	case PROMPT_PLAIN:
	case PROMPT_REPLACE:
	case PROMPT_SHELL:
	case PROMPT_RECT:
		break;
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
	case PROMPT_BUFFER:
		getBufferCompletions(current_text, E.edbuf, &result);
		break;
	case PROMPT_COMMAND:
		getCommandCompletions(current_text, &result);
		break;
	case PROMPT_SEARCH:
		/* For search, we can provide buffer completions */
		getBufferCompletions(current_text, E.edbuf, &result);
		break;
	}

	/* Handle based on number of matches */
	if (result.n_matches == 0) {
		setStatusMessage("[No match]");
		minibuf->completionState.preserve_message = 1;
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
			if (minibuf->completionState.successive_tabs > 0) {
				showCompletionsBuffer(result.matches,
						      result.n_matches, type);
			} else {
				setStatusMessage("[complete, but not unique]");
				minibuf->completionState.preserve_message = 1;
			}
		}
	}

	/* Update state BEFORE cleanup */
	minibuf->completionState.successive_tabs++;
	free(minibuf->completionState.last_completed_text);
	minibuf->completionState.last_completed_text =
		xstrdup((char *)minibuf->row[0].chars);

	/* Cleanup */
	freeCompletionResult(&result);
}

void cycleCompletion(struct buffer *minibuf, int direction) {
	struct completionState *cs = &minibuf->completionState;
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
	struct buffer *b = findBufferByName("*Completions*");
	if (b) {
		b->cy = cs->selected + 2;

		/* Focus never moves to the popup's window during a
		 * prompt (showPopupBuffer() keeps it on the window the
		 * user was editing before the prompt opened), so
		 * scroll() -- which only acts on the focused window --
		 * never notices that b->cy just walked off the visible
		 * range. Without this, the highlighted row keeps
		 * advancing past the window edge while the viewport
		 * sits still. Page up/down already need the same
		 * "find the popup's window" step; see prompt.c. */
		int win_idx = findBufferWindow(b);
		if (win_idx >= 0)
			scrollToShowCursor(E.windows[win_idx], b);
	}
}

/*** Shell prompt completion (#131) ***
 *
 * TAB in the M-! / M-| prompt completes the word that ends at point:
 * an executable on PATH when that word is in command position, a file
 * name anywhere else (and in command position too once the word holds
 * a '/', as in ./configure or /usr/bin/env).
 *
 * The text before point is lexed as sh would read it, far enough to
 * know three things: the word's value with quoting removed, which
 * quote (if any) is open at point, and whether the word is in command
 * position.  Completion only ever inserts at point -- what the user
 * typed is never rewritten -- so the new text is quoted to suit the
 * quote open there: backslashes outside quotes, and the rules of the
 * open quote inside one.  A literal TAB is still C-q TAB. */

struct shellPoint {
	struct dbuf word; /* the word at point, quoting removed */
	int cmd_pos;	  /* that word is in command position */
	int quote;	  /* 0, '\'' or '"': the quote open at point */
	int tilde;	  /* the word began with an unquoted '~' */
	int eq;		  /* offset in word past its last unquoted '=', or -1 */
	int in_word;	  /* point is inside a word rather than between */
	int unusable;	  /* point is in a comment, or after a lone '\' */
};

/* Words after which the next word is again in command position. */
static int isReservedWord(const uint8_t *w, int len) {
	static const char *const words[] = { "!",     "{",    "if", "then",
					     "else",  "elif", "do", "while",
					     "until", "time" };
	for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++)
		if ((int)strlen(words[i]) == len &&
		    memcmp(words[i], w, (size_t)len) == 0)
			return 1;
	return 0;
}

static int isNameChar(uint8_t c, int first) {
	if (c == '_' || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'))
		return 1;
	return !first && c >= '0' && c <= '9';
}

/* Lex s[0..len) -- the prompt's text up to point -- into *p. */
static void lexShellToPoint(const uint8_t *s, int len, struct shellPoint *p) {
	int expect_cmd = 1;   /* the next word starts a command */
	int redir = 0;	      /* the next word is a redirection target */
	int saved_expect = 0; /* expect_cmd to restore after that target */
	int prev_redir = 0;   /* last unquoted byte was '<' or '>' */
	int esc = 0, quote = 0, comment = 0;
	int in_word = 0, word_cmd = 0, plain = 1, name_ok = 1, assign = 0;

	memset(p, 0, sizeof(*p));
	p->eq = -1;

#define START_WORD()                                           \
	do {                                                   \
		if (!in_word) {                                \
			in_word = 1;                           \
			word_cmd = redir ? 0 : expect_cmd;     \
			p->word.len = 0;                       \
			plain = 1;                             \
			name_ok = 1;                           \
			assign = 0;                            \
			p->eq = -1;                            \
			p->tilde = 0;                          \
		}                                              \
	} while (0)

#define END_WORD()                                                         \
	do {                                                               \
		if (in_word) {                                             \
			if (redir) {                                       \
				redir = 0;                                 \
				expect_cmd = saved_expect;                 \
			} else if (word_cmd) {                             \
				expect_cmd =                               \
					assign ||                          \
					(plain &&                          \
					 isReservedWord(p->word.buf,       \
							p->word.len));     \
			}                                                  \
			in_word = 0;                                       \
		}                                                          \
	} while (0)

	for (int i = 0; i < len; i++) {
		uint8_t c = s[i];

		if (comment) {
			if (c == '\n') {
				comment = 0;
				expect_cmd = 1;
			}
			continue;
		}
		if (quote == '\'') {
			if (c == '\'')
				quote = 0;
			else
				dbuf_byte(&p->word, c);
			continue;
		}
		if (quote == '"') {
			if (esc) {
				esc = 0;
				if (c == '\n')
					continue; /* line continuation */
				if (!strchr("$`\"\\", c))
					dbuf_byte(&p->word, '\\');
				dbuf_byte(&p->word, c);
			} else if (c == '\\') {
				esc = 1;
			} else if (c == '"') {
				quote = 0;
			} else {
				dbuf_byte(&p->word, c);
			}
			continue;
		}
		if (esc) {
			esc = 0;
			if (c == '\n')
				continue; /* line continuation */
			START_WORD();
			dbuf_byte(&p->word, c);
			prev_redir = 0;
			continue;
		}

		int was_redir = prev_redir;
		prev_redir = 0;
		switch (c) {
		case '\\':
			START_WORD();
			esc = 1;
			plain = 0;
			name_ok = 0;
			break;
		case '\'':
		case '"':
			START_WORD();
			quote = c;
			plain = 0;
			name_ok = 0;
			break;
		case ' ':
		case '\t':
			END_WORD();
			break;
		case '&':
		case '|':
			/* >& and >| are part of the redirection. */
			if (was_redir)
				break;
			/* fall through */
		case '\n':
		case ';':
		case '(':
		case '`':
			END_WORD();
			redir = 0;
			expect_cmd = 1;
			break;
		case ')':
			END_WORD();
			redir = 0;
			expect_cmd = 0;
			break;
		case '<':
		case '>': {
			/* The 2 of 2>file names a descriptor; it is not
			 * a word and so not the command. */
			int fd_digits = in_word && plain && p->word.len > 0;
			for (int k = 0; fd_digits && k < p->word.len; k++)
				if (p->word.buf[k] < '0' ||
				    p->word.buf[k] > '9')
					fd_digits = 0;
			if (fd_digits)
				in_word = 0;
			else
				END_WORD();
			if (!redir) {
				saved_expect = expect_cmd;
				redir = 1;
			}
			prev_redir = 1;
			break;
		}
		case '#':
			if (!in_word) {
				comment = 1;
				break;
			}
			/* fall through */
		default:
			START_WORD();
			if (c == '=') {
				if (name_ok && p->word.len > 0 && p->eq < 0)
					assign = 1;
				p->eq = p->word.len + 1;
			} else if (p->eq < 0 && name_ok &&
				   !isNameChar(c, p->word.len == 0)) {
				name_ok = 0;
			}
			if (c == '~' && p->word.len == 0)
				p->tilde = 1;
			dbuf_byte(&p->word, c);
			break;
		}
	}
#undef START_WORD
#undef END_WORD

	dbuf_byte(&p->word, '\0'); /* terminate; not counted */
	p->word.len--;
	p->quote = quote;
	p->in_word = in_word;
	p->unusable = comment || esc;
	if (!in_word) {
		p->word.len = 0;
		p->word.buf[0] = '\0';
		p->eq = -1;
		p->tilde = 0;
		p->cmd_pos = redir ? 0 : expect_cmd;
	} else {
		p->cmd_pos = word_cmd;
	}
}

struct shellCand {
	char *full; /* the completed value, quoting removed */
	char *show; /* what the *Completions* list shows */
};

struct shellCands {
	struct shellCand *v;
	int n, cap;
};

static void candsAdd(struct shellCands *c, const char *prefix, int plen,
		     const char *name, const char *tail) {
	if (c->n == c->cap) {
		c->cap = c->cap ? c->cap * 2 : 16;
		c->v = xrealloc(c->v, (size_t)c->cap * sizeof(*c->v));
	}
	size_t nlen = strlen(name), tlen = strlen(tail);
	char *full = xmalloc((size_t)plen + nlen + tlen + 1);
	memcpy(full, prefix, (size_t)plen);
	memcpy(full + plen, name, nlen);
	memcpy(full + plen + nlen, tail, tlen + 1);
	char *show = xmalloc(nlen + tlen + 1);
	memcpy(show, name, nlen);
	memcpy(show + nlen, tail, tlen + 1);
	c->v[c->n].full = full;
	c->v[c->n].show = show;
	c->n++;
}

static void candsFree(struct shellCands *c) {
	for (int i = 0; i < c->n; i++) {
		free(c->v[i].full);
		free(c->v[i].show);
	}
	free(c->v);
	c->v = NULL;
	c->n = c->cap = 0;
}

static int candCmp(const void *a, const void *b) {
	return strcmp(((const struct shellCand *)a)->full,
		      ((const struct shellCand *)b)->full);
}

/* Sort, and drop repeats: a name in two PATH directories is one
 * command, and the shell runs the first. */
static void candsSortUnique(struct shellCands *c) {
	if (c->n == 0)
		return;
	qsort(c->v, (size_t)c->n, sizeof(*c->v), candCmp);
	int dst = 1;
	for (int i = 1; i < c->n; i++) {
		if (strcmp(c->v[i].full, c->v[dst - 1].full) == 0) {
			free(c->v[i].full);
			free(c->v[i].show);
		} else {
			c->v[dst++] = c->v[i];
		}
	}
	c->n = dst;
}

/* A name the minibuffer can hold as typed text: valid UTF-8 and no
 * control characters.  A newline would split the prompt into rows,
 * and an escape would reach the terminal through the status line. */
static int nameInsertable(const char *name) {
	for (const unsigned char *q = (const unsigned char *)name; *q; q++)
		if (*q < 0x20 || *q == 0x7f)
			return 0;
	return utf8_validate((const uint8_t *)name, (int)strlen(name));
}

/* Join dir and name into a malloc'd path for stat(). */
static char *joinPath(const char *dir, const char *name) {
	size_t dlen = strlen(dir), nlen = strlen(name);
	int slash = dlen > 0 && dir[dlen - 1] != '/';
	char *out = xmalloc(dlen + (size_t)slash + nlen + 1);
	memcpy(out, dir, dlen);
	if (slash)
		out[dlen] = '/';
	memcpy(out + dlen + slash, name, nlen + 1);
	return out;
}

/* Files whose path starts with 'value'.  Read with readdir, not glob:
 * the value is literal text, and a '[' or '*' in a file name must not
 * be taken as a pattern.  Directories carry a trailing '/'. */
static void shellFileCands(const char *value, int tilde,
			   struct shellCands *out) {
	if (tilde && strcmp(value, "~") == 0) {
		candsAdd(out, "", 0, "~", "/");
		return;
	}

	char *look = (tilde && strncmp(value, "~/", 2) == 0) ?
			     expandTilde(value) :
			     xstrdup(value);
	const char *slash = strrchr(look, '/');
	const char *base = slash ? slash + 1 : look;
	size_t blen = strlen(base);
	char *dir;
	if (slash) {
		size_t dlen = (size_t)(slash - look) + 1;
		dir = xmalloc(dlen + 1);
		memcpy(dir, look, dlen);
		dir[dlen] = '\0';
	} else {
		dir = xstrdup(".");
	}

	/* The typed text up to the base name, kept as typed (~ and all)
	 * so every candidate extends exactly what is in the prompt. */
	const char *vslash = strrchr(value, '/');
	int vplen = vslash ? (int)(vslash - value) + 1 : 0;

	DIR *d = opendir(dir);
	if (d) {
		struct dirent *de;
		while ((de = readdir(d)) != NULL) {
			const char *name = de->d_name;
			if (strcmp(name, ".") == 0)
				continue;
			if (strcmp(name, "..") == 0 && strcmp(base, "..") != 0)
				continue;
			if (name[0] == '.' && base[0] != '.')
				continue;
			if (strncmp(name, base, blen) != 0)
				continue;
			if (!nameInsertable(name))
				continue;
			char *path = joinPath(dir, name);
			struct stat st;
			int is_dir = stat(path, &st) == 0 &&
				     S_ISDIR(st.st_mode);
			free(path);
			candsAdd(out, value, vplen, name, is_dir ? "/" : "");
		}
		closedir(d);
	}
	free(dir);
	free(look);
}

/* Executables on PATH whose name starts with 'value'. */
static void shellCommandCands(const char *value, struct shellCands *out) {
	const char *path = getenv("PATH");
	if (path == NULL)
		return;
	size_t vlen = strlen(value);

	const char *p = path;
	for (;;) {
		const char *colon = strchr(p, ':');
		size_t len = colon ? (size_t)(colon - p) : strlen(p);
		/* An empty PATH entry means the current directory. */
		char *dir = len ? xmalloc(len + 1) : xstrdup(".");
		if (len) {
			memcpy(dir, p, len);
			dir[len] = '\0';
		}

		DIR *d = opendir(dir);
		if (d) {
			struct dirent *de;
			while ((de = readdir(d)) != NULL) {
				const char *name = de->d_name;
				if (name[0] == '.' && value[0] != '.')
					continue;
				if (strncmp(name, value, vlen) != 0)
					continue;
				if (!nameInsertable(name))
					continue;
				char *full = joinPath(dir, name);
				struct stat st;
				int ok = stat(full, &st) == 0 &&
					 S_ISREG(st.st_mode) &&
					 access(full, X_OK) == 0;
				free(full);
				if (ok)
					candsAdd(out, "", 0, name, "");
			}
			closedir(d);
		}
		free(dir);
		if (!colon)
			break;
		p = colon + 1;
	}
}

/* Append 'text' to d quoted for where it lands: inside the quote that
 * is open at point, or bare.  'word_start' is true when text begins a
 * new word, where a leading '~' would otherwise expand. */
static void appendShellQuoted(struct dbuf *d, const char *text, int quote,
			      int word_start) {
	for (const char *q = text; *q; q++) {
		uint8_t c = (uint8_t)*q;
		if (quote == '\'') {
			if (c == '\'')
				dbuf_append(d, (const uint8_t *)"'\\''", 4);
			else
				dbuf_byte(d, c);
		} else if (quote == '"') {
			if (strchr("\"\\$`", c))
				dbuf_byte(d, '\\');
			dbuf_byte(d, c);
		} else {
			if (strchr(" \t\\'\"`$&|;<>()*?[]#!{}", c) ||
			    (c == '~' && word_start && q == text))
				dbuf_byte(d, '\\');
			dbuf_byte(d, c);
		}
	}
}

/* Length of the longest common prefix of every candidate, cut back to
 * a character boundary so a partial completion never ends mid-UTF-8. */
static int candsCommonLen(const struct shellCands *c) {
	int len = (int)strlen(c->v[0].full);
	for (int i = 1; i < c->n; i++) {
		int k = 0;
		while (k < len && c->v[i].full[k] == c->v[0].full[k])
			k++;
		len = k;
	}
	while (len > 0 && ((uint8_t)c->v[0].full[len] & 0xC0) == 0x80)
		len--;
	return len;
}

/* Point as a byte offset into minibufJoin(mb, "\n"). */
static int minibufPointOffset(struct buffer *mb) {
	int off = 0;
	for (int i = 0; i < mb->cy && i < mb->numrows; i++)
		off += mb->row[i].size + 1;
	return off + mb->cx;
}

void handleShellCompletion(struct buffer *minibuf) {
	struct completionState *cs = &minibuf->completionState;

	/* successive_tabs counts TABs that changed nothing.  Anything
	 * else the user did in between -- typing resets the state in
	 * editorPrompt, and C-p/C-n are caught by the point check --
	 * starts the count again. */
	char *joined = minibufJoin(minibuf, "\n");
	int point = minibufPointOffset(minibuf);
	if (cs->last_completed_text == NULL ||
	    strcmp(joined, cs->last_completed_text) != 0 ||
	    cs->completion_start_pos != point)
		resetCompletionState(cs);

	struct shellPoint sp;
	lexShellToPoint((const uint8_t *)joined, point, &sp);
	free(joined);

	struct shellCands cands = { NULL, 0, 0 };
	const char *value = (const char *)sp.word.buf;
	int tilde = sp.tilde;
	int cmd_pos = sp.cmd_pos;

	if (!sp.unusable) {
		for (;;) {
			if (cmd_pos && strchr(value, '/') == NULL &&
			    !(tilde && strcmp(value, "~") == 0))
				shellCommandCands(value, &cands);
			else
				shellFileCands(value, tilde, &cands);
			if (cands.n > 0 || sp.eq < 0 ||
			    value != (const char *)sp.word.buf)
				break;
			/* Nothing for the whole word: try what follows its
			 * last '=', as in --file=src/ma or PREFIX=~/lo. */
			value = (const char *)sp.word.buf + sp.eq;
			tilde = value[0] == '~';
			cmd_pos = 0;
		}
	}
	candsSortUnique(&cands);

	int vlen = (int)strlen(value);
	struct dbuf ins = DBUF_INIT;
	int step_over_space = 0;

	if (cands.n == 0) {
		setStatusMessage("[No match]");
		cs->preserve_message = 1;
	} else {
		int common = candsCommonLen(&cands);
		const char *full0 = cands.v[0].full;
		char *suffix = NULL;
		if (common > vlen) {
			suffix = xmalloc((size_t)(common - vlen) + 1);
			memcpy(suffix, full0 + vlen, (size_t)(common - vlen));
			suffix[common - vlen] = '\0';
			appendShellQuoted(&ins, suffix, sp.quote,
					  !sp.in_word && sp.quote == 0);
		}
		free(suffix);

		if (cands.n == 1) {
			/* A finished word: close the quote and move on to
			 * the next one, as a shell does.  A directory stays
			 * open so TAB can carry on into it. */
			size_t flen = strlen(full0);
			if (flen == 0 || full0[flen - 1] != '/') {
				struct erow *row = &minibuf->row[minibuf->cy];
				if (sp.quote)
					dbuf_byte(&ins, (uint8_t)sp.quote);
				/* Completing mid-line, before a space
				 * already there: step over it. */
				if (!sp.quote && minibuf->cx < row->size &&
				    row->chars[minibuf->cx] == ' ')
					step_over_space = 1;
				else
					dbuf_byte(&ins, ' ');
			}
			closeCompletionsBuffer();
		} else if (ins.len > 0) {
			closeCompletionsBuffer();
		} else if (cs->successive_tabs > 0) {
			char **shows =
				xmalloc((size_t)cands.n * sizeof(char *));
			for (int i = 0; i < cands.n; i++)
				shows[i] = cands.v[i].show;
			showCompletionsBuffer(shows, cands.n, PROMPT_SHELL);
			free(shows);
		} else {
			setStatusMessage("[complete, but not unique]");
			cs->preserve_message = 1;
		}
	}

	if (ins.len > 0) {
		int ex = minibuf->cx, ey = minibuf->cy;
		mutateInsert(minibuf, minibuf->cx, minibuf->cy, ins.buf,
			     ins.len, &ex, &ey);
		minibuf->cx = ex;
		minibuf->cy = ey;
	}
	if (step_over_space)
		minibuf->cx++;
	dbuf_free(&ins);
	candsFree(&cands);
	dbuf_free(&sp.word);

	cs->successive_tabs++;
	free(cs->last_completed_text);
	cs->last_completed_text = minibufJoin(minibuf, "\n");
	cs->completion_start_pos = minibufPointOffset(minibuf);
}
