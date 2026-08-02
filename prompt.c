/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include "prompt.h"
#include "wrap.h"
#include "buffer.h"
#include "completion.h"
#include "display.h"
#include "edit.h"
#include "emil.h"
#include "history.h"
#include "keymap.h"

#include "terminal.h"
#include "unicode.h"
#include "util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* The minibuffer is a real buffer.  C-q C-j splits it into rows exactly
 * as in any other buffer, so no row anywhere in emil ever holds a
 * literal 0x0A -- the invariant that keeps undo's row arithmetic and
 * every serialization ('\n' as row separator) honest.
 *
 * It is *drawn* on one line, with row breaks in caret notation, which
 * is what Emacs shows when a multi-line entry is recalled into a
 * replace prompt.  Model and rendering are separate choices here.
 *
 * Hence two serializations, both returning malloc'd strings:
 *   minibufJoin(mb, "\n")  the value handed back to the caller
 *   minibufJoin(mb, "^J")  what the user sees
 */
char *minibufJoin(struct buffer *mb, const char *sep) {
	size_t seplen = strlen(sep);
	size_t total = 1;
	for (int i = 0; i < mb->numrows; i++) {
		total += mb->row[i].size;
		if (i + 1 < mb->numrows)
			total += seplen;
	}
	char *out = xmalloc(total);
	size_t at = 0;
	for (int i = 0; i < mb->numrows; i++) {
		if (mb->row[i].chars && mb->row[i].size > 0) {
			memcpy(out + at, mb->row[i].chars, mb->row[i].size);
			at += mb->row[i].size;
		}
		if (i + 1 < mb->numrows) {
			memcpy(out + at, sep, seplen);
			at += seplen;
		}
	}
	out[at] = '\0';
	return out;
}

/* Empty means the *joined value* has zero length: exactly the state
 * where RET should submit "".  Two or more rows is a real entry even
 * when every row is blank -- typing only C-q C-j means the string
 * "\n", the canonical way to join lines in a replace command.  The
 * per-row test used here before swallowed that entry as "". */
static int minibufEmpty(struct buffer *mb) {
	if (mb->numrows > 1)
		return 0;
	return mb->numrows == 0 || mb->row[0].size == 0;
}

/* Copy 's' with each '\n' rewritten as the two bytes "^J", the same
 * caret notation minibufJoin uses for display.  For embedding user
 * text into a prompt or status string: E.statusmsg reaches the
 * terminal raw, so a literal 0x0A there executes as a line feed while
 * stringWidth charges it two columns -- the display and the cursor
 * math disagree, and the minibuffer visibly breaks in two.  Returns a
 * malloc'd string; caller frees. */
char *caretEscapeNewlines(const uint8_t *s) {
	size_t len = 0, extra = 0;
	for (const uint8_t *p = s; *p; p++, len++)
		if (*p == '\n')
			extra++;
	char *out = xmalloc(len + extra + 1);
	char *o = out;
	for (const uint8_t *p = s; *p; p++) {
		if (*p == '\n') {
			*o++ = '^';
			*o++ = 'J';
		} else {
			*o++ = (char)*p;
		}
	}
	*o = '\0';
	return out;
}

/* The one place a prompt type maps to a history ring.  NULL means the
 * prompt keeps no history.  No default: -Wswitch turns a new enum
 * value into a build break here. */
static struct history *histFor(enum promptType t) {
	switch (t) {
	case PROMPT_FILES:
	case PROMPT_DIR:
		return &E.file_history;
	case PROMPT_COMMAND:
		return &E.command_history;
	case PROMPT_BUFFER:
		return &E.buffer_history;
	case PROMPT_REPLACE:
		return &E.replace_history;
	case PROMPT_SHELL:
		return &E.shell_history;
	case PROMPT_RECT:
		return &E.rect_history;
	case PROMPT_SEARCH:
		return &E.search_history;
	case PROMPT_PLAIN:
		return NULL;
	}
	return NULL;
}

/* Display column of point, charging each row break the two columns its
 * "^J" occupies on screen. */
static int minibufCursorCols(struct buffer *mb) {
	int cols = 0;
	for (int i = 0; i < mb->cy && i < mb->numrows; i++)
		cols += stringWidth(mb->row[i].chars) + 2;
	if (mb->cy >= 0 && mb->cy < mb->numrows)
		cols += charsToDisplayColumn(&mb->row[mb->cy], mb->cx);
	return cols;
}

uint8_t *editorPrompt(struct buffer *bufr, const char *prompt,
		      enum promptType t,
		      void (*callback)(struct buffer *, uint8_t *, int)) {
	/* 'prompt' is a plain prefix string displayed before the
	 * minibuffer content.  It is NEVER used as a printf format:
	 * user-controlled text (search terms, filenames) may be
	 * embedded in it freely by callers without escaping.  */
	uint8_t *result = NULL;
	int history_pos = -1;

	/* The text the user typed before starting to browse history.
	 * history_pos == -1 means "showing your own input rather than a
	 * history entry", but nothing preserved that input, so stepping
	 * into history and back out replaced it with the empty string --
	 * as did a bare Down, which never leaves -1 at all.
	 *
	 * Local rather than a field on struct config, deliberately:
	 * prompts nest, and a shared slot would be the same class of bug
	 * as E.edbuf was. */
	char *pending_input = NULL;

	/* Publish the prompt type for keymap.c (quoted-newline
	 * refusal).  Saved and restored so a nested prompt -- e.g.
	 * query-replace's C-r opening a replacement prompt -- does
	 * not clobber its parent's type. */
	enum promptType saved_prompt_type = E.prompt_type;
	E.prompt_type = t;

	replaceMinibufferText(E.minibuf, "");

	/* Save editor buffer and switch to minibuffer.
	 *
	 * E.edbuf must be saved and restored for the same reason
	 * E.prompt_type is.  A prompt can open another prompt --
	 * the loop below dispatches ordinary commands, so C-x C-f,
	 * M-x, C-x b, C-x i, C-x C-w and M-| are all reachable from
	 * inside one -- and on entry the inner prompt would store
	 * the *minibuffer* into the single global slot.  The outer
	 * prompt then restored E.buf = E.edbuf = E.minibuf, leaving
	 * every later keystroke editing the minibuffer object while
	 * the windows still showed the real file. */
	struct buffer *saved_edbuf = E.edbuf;
	E.edbuf = E.buf;
	E.buf = E.minibuf;

	while (1) {
		/* Display prompt with minibuffer content */
		char *shown = minibufJoin(E.minibuf, "^J");
		if (!E.minibuf->completionState.preserve_message) {
			setStatusMessage("%s%s", prompt, shown);
		}
		free(shown);
		E.minibuf->completionState.preserve_message = 0;

		refreshScreen();

		/* Position cursor on bottom line.  cursorBottomLine
		 * expects a display column; E.minibuf->cx is a byte
		 * index, so convert (a CJK character is 3 bytes but 2
		 * columns; passing bytes drifts the cursor right of
		 * the text). */
		int prompt_width = stringWidth((const uint8_t *)prompt);
		cursorBottomLine(prompt_width + minibufCursorCols(E.minibuf) +
				 1);

		/* Read key */
		int c = readKey();
		if (c == -1) {
			/* Interrupted by a signal (suspend/resume,
			 * resize).  The main loop skips these; doing
			 * anything else here would record -1 into a
			 * running macro and feed -1 to the callback. */
			continue;
		}
		recordKey(c);

		int callback_key = c;

		/* Handle special minibuffer keys */
		switch (c) {
		case '\r':
		case CTRL('j'): {
			/* C-j submits: minibuffer-local-map binds both \r
			 * and \n to exit-minibuffer.  Emacs's RET/C-j
			 * divergence exists only in completion maps, and
			 * emil's completion model is its own.  A literal
			 * newline is entered with C-q C-j. */
			if (!minibufEmpty(E.minibuf)) {
				char *current_text =
					minibufJoin(E.minibuf, "\n");

				/* Determine the effective path: if a completion is selected,
				 * use that; otherwise use the minibuffer text. */
				struct completionState *cs =
					&E.minibuf->completionState;
				char *effective_path = current_text;
				if (cs->matches && cs->selected >= 0 &&
				    cs->selected < cs->n_matches) {
					effective_path =
						cs->matches[cs->selected];
				}

				/* Check if this is a file prompt and the path is a directory */
				struct stat st;
				char *stat_path = expandTilde(effective_path);
				int is_dir = (t == PROMPT_FILES &&
					      stat(stat_path, &st) == 0 &&
					      S_ISDIR(st.st_mode));
				free(stat_path);
				if (is_dir) {
					/* User hit Enter on a directory.
					 * Replace minibuffer with the directory path,
					 * append / if needed, and trigger completion. */
					int elen = strlen(effective_path);
					/* Read the last byte BEFORE the
					 * replace/reset below.
					 */
					int ends_slash =
						(elen > 0 &&
						 effective_path[elen - 1] ==
							 '/');

					/* Replace minibuffer content with effective path */
					if (effective_path != current_text) {
						replaceMinibufferText(
							E.minibuf,
							effective_path);
						resetCompletionState(cs);
					}

					if (elen > 0 && !ends_slash) {
						E.minibuf->cx =
							E.minibuf->row[0].size;
						insertChar(E.minibuf, '/', 1);
					}

					handleMinibufferCompletion(E.minibuf,
								   t);
					free(current_text);
					break; /* Do NOT return; keep the user in the prompt */
				}

				/* PROMPT_DIR: strip trailing slash before returning */
				if (t == PROMPT_DIR) {
					int len = strlen(effective_path);
					if (len > 1 &&
					    effective_path[len - 1] == '/') {
						if (effective_path ==
						    current_text) {
							current_text[len - 1] =
								'\0';
						}
					}
				}

				/* Return the effective path */
				result = (uint8_t *)xstrdup(effective_path);
				free(current_text);
			} else {
				result = (uint8_t *)xstrdup("");
			}
			goto done;
		}

		case CTRL('g'):
		case CTRL('c'):
			result = NULL;
			goto done;

		case '\t':
			if (t == PROMPT_FILES || t == PROMPT_DIR ||
			    t == PROMPT_COMMAND || t == PROMPT_BUFFER) {
				handleMinibufferCompletion(E.minibuf, t);
			} else {
				insertChar(E.minibuf, '\t', 1);
			}
			break;

		case CTRL('s'):
		case CTRL('r'):
			/* C-s C-s or C-r C-r: populate empty search with
			 * the last search string. */
			if (t == PROMPT_SEARCH && E.minibuf->numrows > 0 &&
			    E.minibuf->row[0].size == 0) {
				char *last_search = NULL;
				struct historyEntry *last_entry =
					getLastHistory(&E.search_history);
				if (last_entry)
					last_search = last_entry->str;
				if (last_search) {
					replaceMinibufferText(E.minibuf,
							      last_search);
				} else {
					setStatusMessage(
						"[No previous search]");
				}
			}
			break;

		case KEY_META('g'):
			/* Swallow M-g so that the Emacs M-g M-g chord for
			 * goto-line doesn't recursively open a nested
			 * prompt.  The first M-g already opened this one. */
			break;

		case KEY_ARROW_UP:
		case KEY_META('p'):
		case KEY_ARROW_DOWN:
		case KEY_META('n'): {
			/* If completions are visible, cycle selection
			 * instead of history. */
			int down = (c == KEY_ARROW_DOWN || c == KEY_META('n'));
			if (E.minibuf->completionState.matches &&
			    E.minibuf->completionState.n_matches > 0) {
				cycleCompletion(E.minibuf, down ? 1 : -1);
				break;
			}

			struct history *hist = histFor(t);
			char *history_str = NULL;

			if (hist && hist->count > 0) {
				/* Whether a history entry -- rather than the
				 * user's own input -- is currently on show. */
				int was_browsing = (history_pos >= 0);

				if (!down) {
					if (history_pos == -1) {
						/* Leaving the user's own input
						 * for the first time: stash it
						 * so Down can bring it back. */
						free(pending_input);
						pending_input = minibufJoin(
							E.minibuf, "\n");
						history_pos = hist->count - 1;
					} else if (history_pos > 0) {
						history_pos--;
					}
				} else {
					if (history_pos >= 0 &&
					    history_pos < hist->count - 1) {
						history_pos++;
					} else {
						history_pos = -1;
					}
				}

				if (history_pos >= 0) {
					struct historyEntry *entry =
						getHistoryAt(hist, history_pos);
					if (entry)
						history_str = entry->str;
					if (history_str) {
						replaceMinibufferText(
							E.minibuf, history_str);
					}
				} else if (was_browsing) {
					/* Stepped off the end of history and
					 * back to the user's own input. */
					replaceMinibufferText(
						E.minibuf,
						pending_input ? pending_input :
								"");
				}
				/* Otherwise Down was pressed without ever
				 * having browsed: the user's input is
				 * already on show and there is nothing
				 * below it.  Leave the text alone -- this
				 * fell through to a clear before, wiping
				 * whatever had been typed. */
			}
			break;
		}

		default: {
			/* C-p / C-n move the cursor inside the minibuffer;
			 * they should NOT destroy visible completions. */
			int cmd_peek = resolveBinding(c);
			int is_cursor_move = (cmd_peek == CMD_PREV_LINE ||
					      cmd_peek == CMD_NEXT_LINE);

			if (!is_cursor_move &&
			    E.minibuf->completionState.last_completed_text !=
				    NULL) {
				resetCompletionState(
					&E.minibuf->completionState);
			}

			/* Dispatch */
			if (c >= ' ' && c < KEY_ARROW_LEFT)
				E.self_insert_key = c;
			if (cmd_peek != CMD_NONE)
				processKeypress(cmd_peek);

			/* No single-row collapse here.  It concatenated
			 * rows with no separator, so C-y of a multi-line
			 * kill silently ran the lines together, and it
			 * dropped point at end-of-line.  Rows are now the
			 * model; minibufJoin serializes at the edges. */
		}
		}

		if (callback) {
			char *text = E.minibuf->numrows > 0 ?
					     (char *)E.minibuf->row[0].chars :
					     "";
			callback(bufr, (uint8_t *)text, callback_key);
		}
	}

done:
	free(pending_input);

	if (result && strlen((char *)result) > 0) {
		struct history *hist = histFor(t);
		if (hist) {
			addHistory(hist, (char *)result);
		}
	}

	closeCompletionsBuffer();

	E.buf = E.edbuf;
	E.edbuf = saved_edbuf;
	E.prompt_type = saved_prompt_type;

	clearStatusMessage();

	return result;
}
