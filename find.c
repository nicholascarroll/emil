/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include "find.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "history.h"
#include "keymap.h"

#include "prompt.h"
#include "region.h"
#include "terminal.h"
#include "transform.h"
#include "undo.h"
#include "unicode.h"
#include "util.h"
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* File-scope state for the replace family.  These are effectively
 * closure variables for transformerReplaceString, which is passed as
 * a function pointer to transformRegion (signature: uint8_t *(*)(uint8_t *),
 * no user-data slot).  Keeping them static confines the symbol.
 */
static uint8_t *replace_orig;
static uint8_t *replace_repl;

static int regex_mode = 0;

static int initial_direction = 1;

/* Compiled-pattern cache for regexSearch. The one live regex_t is 
 * intentionally left allocated at exit. */
static char *re_cache_pat = NULL;
static regex_t re_cache;
static int re_cache_ok = 0;

/* Compile 'pattern' if it is not already the cached one.  Returns
 * nonzero when the cached pattern is usable. */
static int regexCacheEnsure(const uint8_t *pattern) {
	if (!re_cache_pat || strcmp(re_cache_pat, (const char *)pattern) != 0) {
		if (re_cache_pat) {
			if (re_cache_ok)
				regfree(&re_cache);
			free(re_cache_pat);
		}
		re_cache_pat = xstrdup((const char *)pattern);
		re_cache_ok = (regcomp(&re_cache, (const char *)pattern,
				       REG_EXTENDED) == 0);
	}
	return re_cache_ok;
}

/* On a match, *match_len receives the byte length actually matched. */
static uint8_t *regexSearch(uint8_t *text, uint8_t *pattern, int *match_len) {
	*match_len = 0;
	if (!pattern || !text || pattern[0] == '\0') {
		return NULL;
	}

	if (!regexCacheEnsure(pattern))
		return NULL;

	regmatch_t match[1];
	if (regexec(&re_cache, (const char *)text, 1, match, 0) == 0) {
		*match_len = (int)(match[0].rm_eo - match[0].rm_so);
		return text + match[0].rm_so;
	}

	return NULL;
}

/* Replace all occurrences of 'rep' in 'text' with 'with'.
 * Returns a newly allocated string.  Caller frees. */
static uint8_t *strReplace(uint8_t *text, uint8_t *rep, uint8_t *with) {
	uint8_t *ins;
	uint8_t *tmp;
	size_t len_rep;
	size_t len_with;
	size_t len_front;
	size_t count;

	if (!text || !rep)
		return NULL;
	len_rep = strlen((const char *)rep);
	if (len_rep == 0)
		return NULL;
	if (!with)
		with = (uint8_t *)"";
	len_with = strlen((const char *)with);

	/* Count occurrences */
	ins = text;
	for (count = 0;
	     (tmp = (uint8_t *)strstr((const char *)ins, (const char *)rep));
	     ++count) {
		ins = tmp + len_rep;
	}

	/* Compute result size with overflow check */
	size_t text_len = strlen((const char *)text);
	size_t result_size;
	if (len_with > len_rep) {
		size_t diff = len_with - len_rep;
		if (count > 0 && diff > (SIZE_MAX - text_len - 1) / count)
			return NULL;
		result_size = text_len + diff * count + 1;
	} else if (len_with < len_rep) {
		size_t diff = len_rep - len_with;
		if (diff * count > text_len)
			return NULL;
		result_size = text_len - diff * count + 1;
	} else {
		result_size = text_len + 1;
	}

	uint8_t *result = xmalloc(result_size);
	tmp = result;

	while (count--) {
		ins = (uint8_t *)strstr((const char *)text, (const char *)rep);
		len_front = ins - text;
		memcpy(tmp, text, len_front);
		tmp += len_front;
		size_t remaining = result_size - (tmp - result);
		int written = snprintf((char *)tmp, remaining, "%s", with);
		if (written >= 0 && (size_t)written < remaining) {
			tmp += written;
		}
		text += len_front + len_rep;
	}
	size_t final_remaining = result_size - (tmp - result);
	snprintf((char *)tmp, final_remaining, "%s", text);
	return result;
}

/*** Interactive search ***/

/* Last match on 'row' that begins strictly before byte offset 'limit',
 * or NULL if there is none.
 *
 * Backward search needs the match *before* point, but the only search
 * primitives available scan forward, so walk forward collecting
 * candidates and keep the last one that still starts before the limit.
 * Advancing by a single byte rather than by the match length keeps
 * overlapping matches (searching "aa" in "aaa") reachable.
 *
 * "Begins strictly before the limit" mirrors the forward convention
 * one block down, where a match must begin strictly after the cursor.
 * Keeping the two symmetric is what makes repeated C-r step backward
 * one match at a time instead of sticking on the match under point. */
static uint8_t *searchRowBackward(erow *row, uint8_t *query, int limit,
				  int regex, int *match_len) {
	if (limit <= 0)
		return NULL;

	uint8_t *best = NULL;
	int best_len = 0;
	uint8_t *p = row->chars;
	uint8_t *end = row->chars + row->size;

	while (p <= end) {
		int mlen = 0;
		uint8_t *match;
		if (regex) {
			match = regexSearch(p, query, &mlen);
		} else {
			match = (uint8_t *)strstr((const char *)p,
						  (const char *)query);
			if (match)
				mlen = (int)strlen((const char *)query);
		}
		if (match == NULL)
			break;
		if (match - row->chars >= limit)
			break;
		best = match;
		best_len = mlen;
		p = match + 1;
	}

	if (best)
		*match_len = best_len;
	return best;
}

void findCallback(struct buffer *bufr, uint8_t *query, int key) {
	static int last_match = -1;
	static int direction = 1;

	if (bufr->query != query) {
		free(bufr->query);
		bufr->query = query ? (uint8_t *)xstrdup((const char *)query) :
				      NULL;
	}
	bufr->match = 0;
	bufr->match_len = 0;

	if (key == CTRL('g') || key == CTRL('c') || key == '\r') {
		last_match = -1;
		direction = 1;
		regex_mode = 0;
		return;
	} else if (key == CTRL('s')) {
		direction = 1;
	} else if (key == CTRL('r')) {
		direction = -1;
	} else {
		last_match = -1;
		direction = initial_direction;
	}

	if (!query || strlen((const char *)query) == 0) {
		return;
	}

	/* editorPrompt rebuilds the status line from the prompt
	 * on every pass, so preserve_message is needed to
	 * keep this visible; the pattern is echoed back into the
	 * message so what was typed stays on screen. */
	if (regex_mode && !regexCacheEnsure(query)) {
		setStatusMessage("Invalid regexp: %s", query);
		if (E.minibuf)
			E.minibuf->completionState.preserve_message = 1;
		return;
	}

	if (last_match == -1)
		direction = initial_direction;
	int current = last_match;
	if (current < 0)
		current = (direction == -1) ? bufr->cy : -1;
	if (current >= 0 && current < bufr->numrows) {
		erow *row = &bufr->row[current];
		uint8_t *match;
		int mlen = 0;
		if (direction == -1) {
			/* Backward: the previous match on this row.  This
			 * block used to run the forward search regardless
			 * of direction, so C-r moved forward past point --
			 * and since 'current' is seeded from bufr->cy only
			 * when direction == -1, C-r was almost the only
			 * way to reach it. */
			match = searchRowBackward(row, query, bufr->cx,
						  regex_mode, &mlen);
		} else if (bufr->cx + 1 >= row->size) {
			match = NULL;
		} else {
			if (regex_mode) {
				match = regexSearch(&(row->chars[bufr->cx + 1]),
						    query, &mlen);
			} else {
				match = (uint8_t *)strstr(
					(const char *)&(
						row->chars[bufr->cx + 1]),
					(const char *)query);
				if (match)
					mlen = (int)strlen((const char *)query);
			}
		}
		if (match) {
			last_match = current;
			bufr->cy = current;
			bufr->cx = match - row->chars;
			while (bufr->cx > 0 &&
			       utf8_isCont(row->chars[bufr->cx])) {
				bufr->cx--;
			}
			bufr->match_len =
				mlen + (int)(match - row->chars) - bufr->cx;
			scroll();
			bufr->match = 1;
			return;
		}
	}
	for (int i = 0; i < bufr->numrows; i++) {
		current += direction;
		if (current == -1)
			current = bufr->numrows - 1;
		else if (current == bufr->numrows)
			current = 0;

		erow *row = &bufr->row[current];
		uint8_t *match;
		int mlen = 0;
		if (direction == -1) {
			/* Stepping backward, so the nearest match on an
			 * earlier row is its *last* one.  Taking the first
			 * would skip every other match on that row: the
			 * same-row block above then finds nothing before
			 * it and steps back another row. */
			match = searchRowBackward(row, query, row->size,
						  regex_mode, &mlen);
		} else if (regex_mode) {
			match = regexSearch(row->chars, query, &mlen);
		} else {
			match = (uint8_t *)strstr((const char *)row->chars,
						  (const char *)query);
			if (match)
				mlen = (int)strlen((const char *)query);
		}
		if (match) {
			last_match = current;
			bufr->cy = current;
			bufr->cx = match - row->chars;
			while (bufr->cx > 0 &&
			       utf8_isCont(row->chars[bufr->cx])) {
				bufr->cx--;
			}
			bufr->match_len =
				mlen + (int)(match - row->chars) - bufr->cx;
			scroll();
			bufr->match = 1;
			break;
		}
	}
}

static void searchInteractive(int direction, int regex,
			      const char *prompt_fmt) {
	setMarkSilent();
	regex_mode = regex;
	initial_direction = direction;
	int saved_cx = E.buf->cx;
	int saved_cy = E.buf->cy;

	uint8_t *query =
		editorPrompt(E.buf, prompt_fmt, PROMPT_SEARCH, findCallback);

	free(E.buf->query);
	E.buf->query = NULL;
	regex_mode = 0;
	if (query) {
		free(query);
	} else {
		E.buf->cx = saved_cx;
		E.buf->cy = saved_cy;
	}
}

void editorFind(void) {
	searchInteractive(1, 0, "Search (C-g to cancel): ");
}

void reverseFind(void) {
	searchInteractive(-1, 0, "Reverse search (C-g to cancel): ");
}

void regexFind(void) {
	searchInteractive(1, 1, "Regex search (C-g to cancel): ");
}

void backwardRegexFind(void) {
	searchInteractive(-1, 1, "Reverse regex search (C-g to cancel): ");
}

/*** Replace ***/

/* Transformer callback for transformRegion.  Uses file-scope
 * replace_orig / replace_repl as the search/replace pair. */
static uint8_t *transformerReplaceString(uint8_t *input) {
	return strReplace(input, replace_orig, replace_repl);
}

/* Find the next occurrence of 'needle' in the buffer, starting from
 * (E.buf->cx, E.buf->cy).  When skip_current is set, the match at
 * the current cursor position is skipped (used after a replacement
 * to advance past the just-replaced site).
 *
 * On match: sets E.buf->cx/cy to match start, sets markx/marky to
 * match end (so transformRegion can operate on the match), returns 1.
 * On no match: returns 0; cursor is at end of buffer. */
static int findNextMatch(uint8_t *needle, int skip_current) {
	int ox = E.buf->cx;
	int oy = E.buf->cy;
	int needle_len = strlen((const char *)needle);

	while (E.buf->cy < E.buf->numrows) {
		erow *row = &E.buf->row[E.buf->cy];
		uint8_t *match = (uint8_t *)strstr(
			(const char *)&(row->chars[E.buf->cx]),
			(const char *)needle);
		if (match) {
			int mx = match - row->chars;
			if (skip_current && mx == ox && E.buf->cy == oy) {
				E.buf->cx = mx + 1;
				continue;
			}
			E.buf->cx = mx;
			E.buf->marky = E.buf->cy;
			E.buf->markx = mx + needle_len;
			return 1;
		}
		E.buf->cx = 0;
		E.buf->cy++;
	}
	return 0;
}

void replaceString(void) {
	replace_orig = editorPrompt(E.buf, "Replace: ", PROMPT_REPLACE, NULL);
	if (replace_orig == NULL) {
		setStatusMessage("Canceled replace-string.");
		return;
	}

	/* Prompt is a plain prefix (see editorPrompt), so no percent
	 * escaping -- but a literal newline in the pattern must be
	 * shown as ^J, not fed raw to the terminal. */
	char *esc = caretEscapeNewlines(replace_orig);
	size_t psz = strlen(esc) + 20;
	char *prompt = xmalloc(psz);
	snprintf(prompt, psz, "Replace %s with: ", esc);
	free(esc);
	replace_repl = editorPrompt(E.buf, prompt, PROMPT_REPLACE, NULL);
	free(prompt);
	if (replace_repl == NULL) {
		free(replace_orig);
		replace_orig = NULL;
		setStatusMessage("Canceled replace-string.");
		return;
	}

	transformRegion(transformerReplaceString);

	free(replace_orig);
	free(replace_repl);
	replace_orig = NULL;
	replace_repl = NULL;
}

/* Build the "Query replacing X with Y:" status line shown during the
 * y/n loop.  The pattern is newline-free (rejected up front), but the
 * replacement may contain one and must be shown as ^J.  Returns a
 * malloc'd string; caller frees. */
static char *qrStatusPrompt(void) {
	char *esc_repl = caretEscapeNewlines(replace_repl);
	size_t sz = strlen((const char *)replace_orig) + strlen(esc_repl) + 32;
	char *prompt = xmalloc(sz);
	snprintf(prompt, sz, "Query replacing %s with %s:", replace_orig,
		 esc_repl);
	free(esc_repl);
	return prompt;
}

void queryReplace(void) {
	replace_orig =
		editorPrompt(E.buf, "Query replace: ", PROMPT_REPLACE, NULL);
	if (replace_orig == NULL) {
		setStatusMessage("Canceled query-replace.");
		return;
	}

	/* findNextMatch locates matches with per-row strstr, so a
	 * pattern containing a newline can never match.  The replace
	 * ring is shared with replace-regexp (as query-replace-history
	 * is in Emacs), so a multi-line entry is one M-p away; without
	 * this check it would exit silently with no message at all.
	 * Refuse up front, before asking for a replacement. */
	if (strchr((const char *)replace_orig, '\n')) {
		free(replace_orig);
		replace_orig = NULL;
		setStatusMessage(
			"query-replace cannot match across lines; use replace-regexp");
		return;
	}

	/* Cap the displayed search string to 78 chars.  The prompt is a
	 * plain prefix (see editorPrompt), so no percent escaping is
	 * needed and %.78s truncation is safe. */
	char prompt_buf[192];
	snprintf(prompt_buf, sizeof(prompt_buf),
		 "Query replace %.78s with: ", replace_orig);
	replace_repl = editorPrompt(E.buf, prompt_buf, PROMPT_REPLACE, NULL);
	if (replace_repl == NULL) {
		free(replace_orig);
		replace_orig = NULL;
		setStatusMessage("Canceled query-replace.");
		return;
	}

	/* Status prompt shown during the y/n loop */
	char *prompt = qrStatusPrompt();
	int bufwidth = stringWidth((const uint8_t *)prompt);

	int savedMx = E.buf->markx;
	int savedMy = E.buf->marky;
	struct undo *first = E.buf->undo;
	E.buf->query = replace_orig;
	int currentIdx = windowFocusedIdx();
	struct window *currentWindow = E.windows[currentIdx];

	if (!findNextMatch(replace_orig, 0))
		goto QR_CLEANUP;

	for (;;) {
		setStatusMessage("%s", prompt);
		refreshScreen();
		cursorBottomLine(bufwidth + 2);

		int c = readKey();
		recordKey(c);
		switch (c) {
		case ' ':
		case 'y':
			transformRegion(transformerReplaceString);
			if (!findNextMatch(replace_orig, 1))
				goto QR_CLEANUP;
			break;
		case CTRL('h'):
		case KEY_BACKSPACE:
		case KEY_DEL:
		case 'n':
			E.buf->cx++;
			if (!findNextMatch(replace_orig, 1))
				goto QR_CLEANUP;
			break;
		case '\r':
		case 'q':
		case 'N':
		case CTRL('g'):
			goto QR_CLEANUP;
		case '.':
			transformRegion(transformerReplaceString);
			goto QR_CLEANUP;
		case '!':
		case 'Y':
			if (E.buf->numrows > 0) {
				E.buf->marky = E.buf->numrows - 1;
				E.buf->markx = E.buf->row[E.buf->marky].size;
			} else {
				E.buf->marky = 0;
				E.buf->markx = 0;
			}
			transformRegion(transformerReplaceString);
			goto QR_CLEANUP;
		case 'u':
			/* Only undo replacements made in THIS session */
			if (E.buf->undo != first) {
				doUndo(E.buf, 1);
				E.buf->markx = E.buf->cx;
				E.buf->marky = E.buf->cy;
				E.buf->cx -= strlen((const char *)replace_orig);
				if (E.buf->cx < 0)
					E.buf->cx = 0;
			}
			break;
		case 'U':
			if (E.buf->undo != first) {
				while (E.buf->undo != first)
					doUndo(E.buf, 1);
				E.buf->markx = E.buf->cx;
				E.buf->marky = E.buf->cy;
				E.buf->cx -= strlen((const char *)replace_orig);
				if (E.buf->cx < 0)
					E.buf->cx = 0;
			}
			break;
		case CTRL('r'): {
			char rprompt[192];
			snprintf(rprompt, sizeof(rprompt),
				 "Replace this %.78s with: ", replace_orig);
			uint8_t *newStr = editorPrompt(E.buf, rprompt,
						       PROMPT_REPLACE, NULL);
			if (newStr != NULL) {
				uint8_t *tmp = replace_repl;
				replace_repl = newStr;
				transformRegion(transformerReplaceString);
				free(newStr);
				replace_repl = tmp;
				if (!findNextMatch(replace_orig, 1))
					goto QR_CLEANUP;
			}
			/* Rebuild status prompt */
			free(prompt);
			prompt = qrStatusPrompt();
			bufwidth = stringWidth((const uint8_t *)prompt);
			break;
		}
		case 'e':
		case 'E': {
			char eprompt[192];
			snprintf(eprompt, sizeof(eprompt),
				 "Query replace %.78s with: ", replace_orig);
			uint8_t *newStr = editorPrompt(E.buf, eprompt,
						       PROMPT_REPLACE, NULL);
			if (newStr != NULL) {
				free(replace_repl);
				replace_repl = newStr;
				transformRegion(transformerReplaceString);
				if (!findNextMatch(replace_orig, 1))
					goto QR_CLEANUP;
			}
			free(prompt);
			prompt = qrStatusPrompt();
			bufwidth = stringWidth((const uint8_t *)prompt);
			break;
		}
		case CTRL('l'):
			recenter(currentWindow);
			break;
		}
	}

QR_CLEANUP:
	clearStatusMessage();
	E.buf->query = NULL;
	E.buf->markx = savedMx;
	E.buf->marky = savedMy;
	free(replace_orig);
	free(replace_repl);
	replace_orig = NULL;
	replace_repl = NULL;
	free(prompt);
}
