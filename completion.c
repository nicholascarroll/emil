/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include <dirent.h>
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
	for (int i = 0; i < result->n_matches; i++)
		free(result->matches[i]);
	free(result->matches);
	free(result->common_prefix);
	memset(result, 0, sizeof(*result));
}

/* Append m, which the result takes ownership of. */
static void pushMatch(struct completionResult *r, char *m) {
	if (r->n_matches == r->cap) {
		r->cap = r->cap ? r->cap * 2 : 16;
		r->matches =
			xrealloc(r->matches, (size_t)r->cap * sizeof(char *));
	}
	r->matches[r->n_matches++] = m;
}

static int cmpMatch(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Sort, and drop repeats: a name in two PATH directories is one
 * command, and the shell runs the first. */
static void sortUniqueMatches(struct completionResult *r) {
	if (r->n_matches == 0)
		return;
	qsort(r->matches, (size_t)r->n_matches, sizeof(char *), cmpMatch);
	int dst = 1;
	for (int i = 1; i < r->n_matches; i++) {
		if (strcmp(r->matches[i], r->matches[dst - 1]) == 0)
			free(r->matches[i]);
		else
			r->matches[dst++] = r->matches[i];
	}
	r->n_matches = dst;
}

/* The longest prefix common to every string, cut back to a character
 * boundary so a partial completion never ends mid-UTF-8. */
static char *findCommonPrefix(char **strings, int count) {
	if (count == 0)
		return NULL;
	int len = (int)strlen(strings[0]);
	for (int i = 1; i < count; i++) {
		int k = 0;
		while (k < len && strings[i][k] == strings[0][k])
			k++;
		len = k;
	}
	while (count > 1 && len > 0 && utf8_isCont((uint8_t)strings[0][len]))
		len--;
	char *prefix = xmalloc((size_t)len + 1);
	memcpy(prefix, strings[0], (size_t)len);
	prefix[len] = '\0';
	return prefix;
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

enum scanKind { SCAN_ANY, SCAN_DIRS, SCAN_EXEC };

/* Add each entry of dir whose name extends base, as typed[0..tlen)
 * followed by the name, and a '/' after a directory.  Read with
 * readdir, not glob: typed text is literal, and a '[' or '*' in a
 * file name must not be taken as a pattern.  Dot files match only a
 * base that starts with a dot. */
static void scanDir(const char *dir, const char *typed, int tlen,
		    const char *base, enum scanKind kind,
		    struct completionResult *r) {
	DIR *d = opendir(dir);
	if (d == NULL)
		return;
	size_t blen = strlen(base), dlen = strlen(dir);
	struct dirent *de;
	while ((de = readdir(d)) != NULL) {
		const char *name = de->d_name;
		if (strncmp(name, base, blen) != 0 ||
		    (name[0] == '.' && base[0] != '.') ||
		    strcmp(name, ".") == 0 ||
		    (strcmp(name, "..") == 0 && strcmp(base, "..") != 0) ||
		    !nameInsertable(name))
			continue;
		size_t nlen = strlen(name);
		char *path = xmalloc(dlen + nlen + 2);
		snprintf(path, dlen + nlen + 2, "%s/%s", dir, name);
		struct stat st;
		int found = stat(path, &st) == 0;
		int is_dir = found && S_ISDIR(st.st_mode);
		int ok = kind == SCAN_ANY || (kind == SCAN_DIRS && is_dir) ||
			 (kind == SCAN_EXEC && found && S_ISREG(st.st_mode) &&
			  access(path, X_OK) == 0);
		free(path);
		if (!ok)
			continue;
		size_t mlen = (size_t)tlen + nlen + 2;
		char *m = xmalloc(mlen);
		snprintf(m, mlen, "%.*s%s%s", tlen, typed, name,
			 is_dir && kind != SCAN_EXEC ? "/" : "");
		pushMatch(r, m);
	}
	closedir(d);
}

/* Files whose path extends value.  Each keeps value's directory part
 * exactly as typed, ~ and all, so it extends the text in the prompt.
 * A leading ~ is expanded only when 'tilde' says it is unquoted. */
static void getFileCompletions(const char *value, int tilde, enum scanKind kind,
			       struct completionResult *r) {
	if (tilde && strcmp(value, "~") == 0) {
		pushMatch(r, xstrdup("~/"));
		return;
	}
	char *look = tilde ? expandTilde(value) : xstrdup(value);
	char *slash = strrchr(look, '/');
	const char *vslash = strrchr(value, '/');
	char *base = xstrdup(slash ? slash + 1 : look);
	if (slash)
		slash[1] = '\0';
	scanDir(slash ? look : ".", value,
		vslash ? (int)(vslash - value) + 1 : 0, base, kind, r);
	free(base);
	free(look);
	sortUniqueMatches(r);
}

/* Executables on PATH whose name starts with value. */
static void getPathCompletions(const char *value, struct completionResult *r) {
	const char *p = getenv("PATH");
	while (p != NULL) {
		size_t len = strcspn(p, ":");
		/* An empty PATH entry means the current directory. */
		int n = len ? (int)len : 1;
		char *dir = xmalloc((size_t)n + 1);
		snprintf(dir, (size_t)n + 1, "%.*s", n, len ? p : ".");
		scanDir(dir, "", 0, value, SCAN_EXEC, r);
		free(dir);
		p = p[len] == ':' ? p + len + 1 : NULL;
	}
	sortUniqueMatches(r);
}

static void getBufferCompletions(const char *prefix,
				 struct buffer *currentBuffer,
				 struct completionResult *result) {
	/* The common prefix is taken over basenames, since the user
	 * types basenames in the prompt. */
	struct completionResult bases = { 0 };
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
		if (b == currentBuffer ||
		    (b->filename && strcmp(b->filename, "*Completions*") == 0))
			continue;
		const char *name = b->filename ? b->filename : "*scratch*";
		const char *slash = strrchr(name, '/');
		const char *base = slash ? slash + 1 : name;
		if (strncmp(base, prefix, strlen(prefix)) == 0) {
			pushMatch(result, xstrdup(name));
			pushMatch(&bases, xstrdup(base));
		}
	}
	result->common_prefix =
		findCommonPrefix(bases.matches, bases.n_matches);
	freeCompletionResult(&bases);
}

/* M-x commands, matched case-insensitively. */
static void getCommandCompletions(const char *prefix,
				  struct completionResult *result) {
	size_t len = strlen(prefix);
	char *lower = xstrdup(prefix);
	for (char *q = lower; *q; q++)
		if (*q >= 'A' && *q <= 'Z')
			*q |= 0x20;
	for (int i = 0; i < E.cmd_count; i++)
		if (strncmp(E.cmd[i].key, lower, len) == 0)
			pushMatch(result, xstrdup(E.cmd[i].key));
	free(lower);
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

/* Point as a byte offset into minibufJoin(mb, "\n"). */
static int minibufPointOffset(struct buffer *mb) {
	int off = 0;
	for (int i = 0; i < mb->cy && i < mb->numrows; i++)
		off += mb->row[i].size + 1;
	return off + mb->cx;
}

/* Start a TAB: return the prompt's text and set *point.
 * successive_tabs counts TABs that changed nothing; typing resets the
 * state in editorPrompt, and moving point (C-p/C-n) is caught here. */
static char *tabBegin(struct buffer *mb, int *point) {
	struct completionState *cs = &mb->completionState;
	char *text = minibufJoin(mb, "\n");
	*point = minibufPointOffset(mb);
	if (cs->last_completed_text == NULL ||
	    strcmp(text, cs->last_completed_text) != 0 ||
	    cs->completion_start_pos != *point)
		resetCompletionState(cs);
	return text;
}

static void tabEnd(struct buffer *mb) {
	struct completionState *cs = &mb->completionState;
	cs->successive_tabs++;
	free(cs->last_completed_text);
	cs->last_completed_text = minibufJoin(mb, "\n");
	cs->completion_start_pos = minibufPointOffset(mb);
}

/* What every prompt's TAB does with the matches for a word of 'typed'
 * bytes: report no match; or, when the common prefix adds nothing and
 * the match is not unique, say so, and list the matches on a second
 * TAB.  Returns 1 when the caller should extend the word to
 * r->common_prefix, which for a unique match is the whole match. */
static int tabOutcome(struct buffer *mb, struct completionResult *r,
		      size_t typed, enum promptType type) {
	struct completionState *cs = &mb->completionState;
	if (r->n_matches > 0 && r->common_prefix == NULL)
		r->common_prefix = findCommonPrefix(r->matches, r->n_matches);
	if (r->n_matches == 1 ||
	    (r->n_matches > 1 && strlen(r->common_prefix) > typed)) {
		closeCompletionsBuffer();
		return 1;
	}
	if (r->n_matches > 1 && cs->successive_tabs > 0) {
		showCompletionsBuffer(r->matches, r->n_matches, type);
		return 0;
	}
	setStatusMessage(r->n_matches ? "[complete, but not unique]" :
					"[No match]");
	cs->preserve_message = 1;
	return 0;
}

void handleMinibufferCompletion(struct buffer *minibuf, enum promptType type) {
	int point;
	char *text = tabBegin(minibuf, &point);
	struct completionResult result = { 0 };
	switch (type) {
	case PROMPT_PLAIN:
	case PROMPT_REPLACE:
	case PROMPT_SHELL:
	case PROMPT_RECT:
		break;
	case PROMPT_FILES:
	case PROMPT_DIR:
		getFileCompletions(text, text[0] == '~',
				   type == PROMPT_DIR ? SCAN_DIRS : SCAN_ANY,
				   &result);
		break;
	case PROMPT_BUFFER:
	case PROMPT_SEARCH:
		getBufferCompletions(text, E.edbuf, &result);
		break;
	case PROMPT_COMMAND:
		getCommandCompletions(text, &result);
		break;
	}
	if (tabOutcome(minibuf, &result, strlen(text), type))
		replaceMinibufferText(minibuf, result.n_matches == 1 ?
						       result.matches[0] :
						       result.common_prefix);
	tabEnd(minibuf);
	freeCompletionResult(&result);
	free(text);
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
	cs->completion_start_pos = minibufPointOffset(minibuf);

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
 * a '/', as in ./configure).  Completion only ever inserts at point,
 * quoted to suit the quote open there.  A literal TAB is C-q TAB. */

struct shellWord {
	struct dbuf text; /* the word before point, quoting removed */
	int cmd;	  /* it is in command position */
	int quote;	  /* 0, '\'' or '"': the quote open at point */
	int tilde;	  /* it began with an unquoted '~' */
	int fresh;	  /* point is between words */
};

/* After one of these as a whole word, a command follows. */
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

/* NAME=..., which leaves the next word in command position. */
static int isAssignment(const uint8_t *w, int len) {
	int i = 0;
	while (i < len &&
	       (w[i] == '_' || ((w[i] | 0x20) >= 'a' && (w[i] | 0x20) <= 'z') ||
		(i > 0 && w[i] >= '0' && w[i] <= '9')))
		i++;
	return i > 0 && i < len && w[i] == '=';
}

/* Lex s[0..len), the prompt up to point, as far as needed to find the
 * word being completed.  | ; & ( ` and newline start a command; < and
 * > make the next word a file.  Returns 0 if point follows a lone '\'. */
static int lexShellWord(const uint8_t *s, int len, struct shellWord *w) {
	int cmd = 1, target = 0, in_word = 0, plain = 0, esc = 0, quote = 0;
	memset(w, 0, sizeof(*w));
	for (int i = 0; i < len; i++) {
		uint8_t c = s[i];
		if (esc || (quote == '\'' && c != '\'') ||
		    (quote == '"' && c != '"' && c != '\\')) {
			if (esc && quote == '"' && !strchr("$`\"\\", c))
				dbuf_byte(&w->text, '\\');
			esc = 0;
			dbuf_byte(&w->text, c);
		} else if (quote) { /* a closing quote, or '\\' inside "" */
			if (c == '\\')
				esc = 1;
			else
				quote = 0;
		} else if (c && strchr(" \t\n|;&()`<>", c)) {
			if (in_word && target)
				target = 0;
			else if (in_word && w->cmd)
				cmd = plain &&
				      (isReservedWord(w->text.buf,
						      w->text.len) ||
				       isAssignment(w->text.buf, w->text.len));
			in_word = 0;
			if (c == '<' || c == '>')
				target = 1;
			else if (c == ')')
				cmd = 0;
			else if (c != ' ' && c != '\t' &&
				 s[i ? i - 1 : 0] != '>') {
				cmd = 1; /* but >& and >| are redirections */
				target = 0;
			}
		} else {
			if (!in_word) {
				in_word = plain = 1;
				w->text.len = 0;
				w->cmd = cmd && !target;
				w->tilde = c == '~';
			}
			/* Quoting after an '=' still allows an assignment. */
			if ((c == '\\' || c == '\'' || c == '"') &&
			    !(w->text.len &&
			      memchr(w->text.buf, '=', (size_t)w->text.len)))
				plain = 0;
			if (c == '\\')
				esc = 1;
			else if (c == '\'' || c == '"')
				quote = c;
			else
				dbuf_byte(&w->text, c);
		}
	}
	if (!in_word) {
		w->text.len = 0;
		w->cmd = cmd && !target;
		w->tilde = 0;
	}
	dbuf_byte(&w->text, '\0'); /* terminate; not counted */
	w->text.len--;
	w->quote = quote;
	w->fresh = !in_word;
	return !esc;
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

void handleShellCompletion(struct buffer *minibuf) {
	int point;
	char *joined = tabBegin(minibuf, &point);
	struct shellWord w;
	int usable = lexShellWord((const uint8_t *)joined, point, &w);
	free(joined);

	struct completionResult r = { 0 };
	const char *value = (const char *)w.text.buf;
	const char *eq = strrchr(value, '=');
	if (usable && w.cmd && !w.tilde && !strchr(value, '/'))
		getPathCompletions(value, &r);
	else if (usable)
		getFileCompletions(value, w.tilde, SCAN_ANY, &r);
	/* Nothing for the whole word: try what follows its last '=', as
	 * in --file=src/ma or PREFIX=~/lo. */
	if (usable && r.n_matches == 0 && eq) {
		value = eq + 1;
		getFileCompletions(value, value[0] == '~', SCAN_ANY, &r);
	}

	size_t vlen = strlen(value);
	struct dbuf ins = DBUF_INIT;
	int step = 0;
	if (tabOutcome(minibuf, &r, vlen, PROMPT_SHELL)) {
		const char *done = r.common_prefix;
		size_t dlen = strlen(done);
		appendShellQuoted(&ins, done + vlen, w.quote, w.fresh);
		/* A finished word: close the quote and move on to the next
		 * one, as a shell does, stepping over a space already there.
		 * A directory stays open so TAB can carry on into it. */
		if (r.n_matches == 1 && (dlen == 0 || done[dlen - 1] != '/')) {
			const struct erow *row = &minibuf->row[minibuf->cy];
			if (w.quote)
				dbuf_byte(&ins, (uint8_t)w.quote);
			else if (minibuf->cx < row->size &&
				 row->chars[minibuf->cx] == ' ')
				step = 1;
			if (!step)
				dbuf_byte(&ins, ' ');
		}
	}
	if (ins.len > 0) {
		int ex = minibuf->cx, ey = minibuf->cy;
		mutateInsert(minibuf, minibuf->cx, minibuf->cy, ins.buf,
			     ins.len, &ex, &ey);
		minibuf->cx = ex;
		minibuf->cy = ey;
	}
	minibuf->cx += step;
	dbuf_free(&ins);
	freeCompletionResult(&r);
	dbuf_free(&w.text);
	tabEnd(minibuf);
}
