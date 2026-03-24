#include "find.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "history.h"
#include "keymap.h"
#include "message.h"
#include "prompt.h"
#include "region.h"
#include "terminal.h"
#include "transform.h"
#include "undo.h"
#include "unicode.h"
#include "unused.h"
#include "util.h"
#include <regex.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct config E;
static int regex_mode = 0;

/* Helper function to search for regex match in a string */
static uint8_t *regexSearch(uint8_t *text, uint8_t *pattern) {
	if (!pattern || !text || strlen((char *)pattern) == 0) {
		return NULL;
	}

	regex_t regex;
	regmatch_t match[1];

	/* Try to compile regex, fall back to literal search if invalid */
	if (regcomp(&regex, (char *)pattern, REG_EXTENDED) != 0) {
		return strstr((char *)text, (char *)pattern);
	}

	/* Execute regex search */
	if (regexec(&regex, (char *)text, 1, match, 0) == 0) {
		regfree(&regex);
		return text + match[0].rm_so;
	}

	regfree(&regex);
	return NULL;
}

// https://stackoverflow.com/a/779960
// You must free the result if result is non-NULL.
uint8_t *orig;
uint8_t *repl;

char *str_replace(char *orig, char *rep, char *with) {
	char *result;	  // the return string
	char *ins;	  // the next insert point
	char *tmp;	  // varies
	size_t len_rep;	  // length of rep (the string to remove)
	size_t len_with;  // length of with (the string to replace rep with)
	size_t len_front; // distance between rep and end of last rep
	size_t count;	  // number of replacements

	// sanity checks and initialization
	if (!orig || !rep)
		return NULL;
	len_rep = strlen(rep);
	if (len_rep == 0)
		return NULL; // empty rep causes infinite loop during count
	if (!with)
		with = "";
	len_with = strlen(with);

	// count the number of replacements needed
	ins = orig;
	for (count = 0; (tmp = strstr(ins, rep)); ++count) {
		ins = tmp + len_rep;
	}

	// Check for potential overflow
	size_t orig_len = strlen(orig);
	size_t result_size;
	if (len_with > len_rep) {
		// Check if multiplication would overflow
		size_t diff = len_with - len_rep;
		if (count > 0 && diff > (SIZE_MAX - orig_len - 1) / count) {
			return NULL; // Overflow would occur
		}
		result_size = orig_len + diff * count + 1;
	} else if (len_with < len_rep) {
		// Shrinking - need to handle underflow
		size_t diff = len_rep - len_with;
		if (diff * count > orig_len) {
			// Would result in negative size
			return NULL;
		}
		result_size = orig_len - diff * count + 1;
	} else {
		// len_with == len_rep, no size change
		result_size = orig_len + 1;
	}
	tmp = result = xmalloc(result_size);

	if (!result)
		return NULL;

	// first time through the loop, all the variable are set correctly
	// from here on,
	//    tmp points to the end of the result string
	//    ins points to the next occurrence of rep in orig
	//    orig points to the remainder of orig after "end of rep"
	while (count--) {
		ins = strstr(orig, rep);
		len_front = ins - orig;
		size_t remaining = result_size - (tmp - result);
		memcpy(tmp, orig, len_front);
		tmp += len_front;
		remaining = result_size - (tmp - result);
		int written = snprintf(tmp, remaining, "%s", with);
		if (written >= 0 && (size_t)written < remaining) {
			tmp += written;
		}
		orig += len_front + len_rep; // move to next "end of rep"
	}
	size_t final_remaining = result_size - (tmp - result);
	snprintf(tmp, final_remaining, "%s", orig);
	return result;
}

static int initial_direction = 1;

void findCallback(struct buffer *bufr, uint8_t *query, int key) {
	static int last_match = -1;
	static int direction = 1;

	if (bufr->query != query) {
		free(bufr->query);
		bufr->query = query ? xstrdup((char *)query) : NULL;
	}
	bufr->match = 0;

	if (key == CTRL('g') || key == CTRL('c') || key == '\r') {
		last_match = -1;
		direction = 1;
		regex_mode = 0; /* Reset regex mode on exit */
		return;
	} else if (key == CTRL('s')) {
		direction = 1;
	} else if (key == CTRL('r')) {
		direction = -1;
	} else {
		last_match = -1;
		direction = initial_direction;
	}

	if (!query || strlen((char *)query) == 0) {
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
		if (bufr->cx + 1 >= row->size) {
			match = NULL;
		} else {
			if (regex_mode) {
				match = regexSearch(&(row->chars[bufr->cx + 1]),
						    query);
			} else {
				match = strstr(
					(char *)&(row->chars[bufr->cx + 1]),
					(char *)query);
			}
		}
		if (match) {
			last_match = current;
			bufr->cy = current;
			bufr->cx = match - row->chars;
			/* Ensure we're at a character boundary */
			while (bufr->cx > 0 &&
			       utf8_isCont(row->chars[bufr->cx])) {
				bufr->cx--;
			}
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
		if (regex_mode) {
			match = regexSearch(row->chars, query);
		} else {
			match = strstr((char *)row->chars, (char *)query);
		}
		if (match) {
			last_match = current;
			bufr->cy = current;
			bufr->cx = match - row->chars;
			/* Ensure we're at a character boundary */
			while (bufr->cx > 0 &&
			       utf8_isCont(row->chars[bufr->cx])) {
				bufr->cx--;
			}
			scroll();
			bufr->match = 1;
			break;
		}
	}
}

void editorFind(void) {
	setMarkSilent();
	regex_mode = 0; /* Start in normal mode */
	initial_direction = 1;
	int saved_cx = E.buf->cx;
	int saved_cy = E.buf->cy;
	//	int saved_rowoff = E.buf->rowoff;

	uint8_t *query = editorPrompt(E.buf, "Search (C-g to cancel): %s",
				      PROMPT_SEARCH, findCallback);

	free(E.buf->query);
	E.buf->query = NULL;
	if (query) {
		free(query);
	} else {
		E.buf->cx = saved_cx;
		E.buf->cy = saved_cy;
		//		E.buf->rowoff = saved_rowoff;
	}
}

void reverseFind(void) {
	setMarkSilent();
	regex_mode = 0;
	initial_direction = -1;
	int saved_cx = E.buf->cx;
	int saved_cy = E.buf->cy;

	uint8_t *query = editorPrompt(E.buf,
				      "Reverse search (C-g to cancel): %s",
				      PROMPT_SEARCH, findCallback);

	free(E.buf->query);
	E.buf->query = NULL;
	if (query) {
		free(query);
	} else {
		E.buf->cx = saved_cx;
		E.buf->cy = saved_cy;
	}
}

void regexFind(void) {
	setMarkSilent();
	regex_mode = 1; /* Start in regex mode */
	int saved_cx = E.buf->cx;
	int saved_cy = E.buf->cy;

	uint8_t *query = editorPrompt(E.buf, "Regex search (C-g to cancel): %s",
				      PROMPT_SEARCH, findCallback);

	free(E.buf->query);
	E.buf->query = NULL;
	regex_mode = 0; /* Reset after search */
	if (query) {
		free(query);
	} else {
		E.buf->cx = saved_cx;
		E.buf->cy = saved_cy;
	}
}

uint8_t *transformerReplaceString(uint8_t *input) {
	return str_replace(input, orig, repl);
}

void replaceString(void) {
	orig = NULL;
	repl = NULL;
	orig = editorPrompt(E.buf, "Replace: %s", PROMPT_BASIC, NULL);
	if (orig == NULL) {
		setStatusMessage(msg_canceled_replace);
		return;
	}

	uint8_t *prompt = xmalloc(strlen(orig) + 20);
	snprintf(prompt, strlen(orig) + 20, "Replace %s with: %%s", orig);
	repl = editorPrompt(E.buf, prompt, PROMPT_BASIC, NULL);
	free(prompt);
	prompt = NULL;
	if (repl == NULL) {
		free(orig);
		setStatusMessage(msg_canceled_replace);
		return;
	}

	transformRegion(transformerReplaceString);

	free(orig);
	free(repl);
}

static int nextOccur(uint8_t *needle, int ocheck) {
	int ox = E.buf->cx;
	int oy = E.buf->cy;
	if (!ocheck) {
		ox = -69;
	}
	while (E.buf->cy < E.buf->numrows) {
		erow *row = &E.buf->row[E.buf->cy];
		uint8_t *match = strstr((char *)&(row->chars[E.buf->cx]),
					(char *)needle);
		if (match) {
			if (!(E.buf->cx == ox && E.buf->cy == oy)) {
				E.buf->cx = match - row->chars;
				E.buf->marky = E.buf->cy;
				E.buf->markx = E.buf->cx + strlen(needle);
				/* E.buf->rowoff = E.buf->numrows; */
				return 1;
			}
			E.buf->cx++;
		}
		E.buf->cx = 0;
		E.buf->cy++;
	}
	return 0;
}

void queryReplace(void) {
	orig = NULL;
	repl = NULL;
	orig = editorPrompt(E.buf, "Query replace: %s", PROMPT_BASIC, NULL);
	if (orig == NULL) {
		setStatusMessage(msg_canceled_query_replace);
		return;
	}

	uint8_t *prompt = xmalloc(strlen(orig) + 25);
	snprintf(prompt, strlen(orig) + 25, "Query replace %s with: %%s", orig);
	repl = editorPrompt(E.buf, prompt, PROMPT_BASIC, NULL);
	free(prompt);
	if (repl == NULL) {
		free(orig);
		setStatusMessage(msg_canceled_query_replace);
		return;
	}

	prompt = xmalloc(strlen(orig) + strlen(repl) + 32);
	snprintf(prompt, strlen(orig) + strlen(repl) + 32,
		 "Query replacing %s with %s:", orig, repl);
	int bufwidth = stringWidth(prompt);
	int savedMx = E.buf->markx;
	int savedMy = E.buf->marky;
	struct undo *first = E.buf->undo;
	uint8_t *newStr = NULL;
	E.buf->query = orig;
	int currentIdx = windowFocusedIdx();
	struct window *currentWindow = E.windows[currentIdx];

#define NEXT_OCCUR(ocheck)            \
	if (!nextOccur(orig, ocheck)) \
	goto QR_CLEANUP

	NEXT_OCCUR(false);

	for (;;) {
		setStatusMessage(prompt);
		refreshScreen();
		cursorBottomLine(bufwidth + 2);

		int c = readKey();
		recordKey(c);
		switch (c) {
		case ' ':
		case 'y':
			transformRegion(transformerReplaceString);
			NEXT_OCCUR(true);
			break;
		case CTRL('h'):
		case KEY_BACKSPACE:
		case KEY_DEL:
		case 'n':
			E.buf->cx++;
			NEXT_OCCUR(true);
			break;
		case '\r':
		case 'q':
		case 'N':
		case CTRL('g'):
			goto QR_CLEANUP;
			break;
		case '.':
			transformRegion(transformerReplaceString);
			goto QR_CLEANUP;
			break;
		case '!':
		case 'Y':
			E.buf->marky = E.buf->numrows - 1;
			E.buf->markx = E.buf->row[E.buf->marky].size;
			transformRegion(transformerReplaceString);
			goto QR_CLEANUP;
			break;
		case 'u':
			doUndo(E.buf, 1);
			E.buf->markx = E.buf->cx;
			E.buf->marky = E.buf->cy;
			E.buf->cx -= strlen(orig);
			break;
		case 'U':
			while (E.buf->undo != first)
				doUndo(E.buf, 1);
			E.buf->markx = E.buf->cx;
			E.buf->marky = E.buf->cy;
			E.buf->cx -= strlen(orig);
			break;
		case CTRL('r'):
			prompt = xmalloc(strlen(orig) + 25);
			snprintf(prompt, strlen(orig) + 25,
				 "Replace this %s with: %%s", orig);
			newStr =
				editorPrompt(E.buf, prompt, PROMPT_BASIC, NULL);
			free(prompt);
			if (newStr == NULL) {
				goto RESET_PROMPT;
			}
			uint8_t *tmp = repl;
			repl = newStr;
			transformRegion(transformerReplaceString);
			free(newStr);
			repl = tmp;
			NEXT_OCCUR(true);
			goto RESET_PROMPT;
			break;
		case 'e':
		case 'E':
			prompt = xmalloc(strlen(orig) + 25);
			snprintf(prompt, strlen(orig) + 25,
				 "Query replace %s with: %%s", orig);
			newStr =
				editorPrompt(E.buf, prompt, PROMPT_BASIC, NULL);
			free(prompt);
			if (newStr == NULL) {
				goto RESET_PROMPT;
			}
			free(repl);
			repl = newStr;
			transformRegion(transformerReplaceString);
			NEXT_OCCUR(true);
RESET_PROMPT:
			prompt = xmalloc(strlen(orig) + strlen(repl) + 32);
			snprintf(prompt, strlen(orig) + strlen(repl) + 32,
				 "Query replacing %s with %s:", orig, repl);
			bufwidth = stringWidth(prompt);
			break;
		case CTRL('l'):
			recenter(currentWindow);
			break;
		}
	}

QR_CLEANUP:
	setStatusMessage("");
	E.buf->query = NULL;
	E.buf->markx = savedMx;
	E.buf->marky = savedMy;
	free(orig);
	free(repl);
	if (prompt != NULL) {
		free(prompt);
	}
}

/* Wrapper for command table */
void regexFindWrapper(void) {
	regexFind();
}

void backwardRegexFind(void) {
	regex_mode = 1; /* Start in regex mode */
	initial_direction = -1;
	int saved_cx = E.buf->cx;
	int saved_cy = E.buf->cy;

	uint8_t *query =
		editorPrompt(E.buf, "Reverse regex search (C-g to cancel): %s",
			     PROMPT_SEARCH, findCallback);

	free(E.buf->query);
	E.buf->query = NULL;
	regex_mode = 0; /* Reset after search */
	if (query) {
		free(query);
	} else {
		E.buf->cx = saved_cx;
		E.buf->cy = saved_cy;
	}
}
