#include "prompt.h"
#include "wrap.h"
#include "buffer.h"
#include "completion.h"
#include "display.h"
#include "edit.h"
#include "emil.h"
#include "history.h"
#include "keymap.h"
#include "message.h"
#include "terminal.h"
#include "unicode.h"
#include "util.h"
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

extern struct config E;

uint8_t *editorPrompt(struct buffer *bufr, const char *prompt,
		      enum promptType t,
		      void (*callback)(struct buffer *, uint8_t *, int)) {
	/* 'prompt' is a plain prefix string displayed before the
	 * minibuffer content.  It is NEVER used as a printf format:
	 * user-controlled text (search terms, filenames) may be
	 * embedded in it freely by callers without escaping.  This is
	 * the fix for the tainted-format-string class (CodeQL
	 * cpp/tainted-format-string): keeping runtime data out of the
	 * format-argument position entirely, rather than escaping it
	 * at each call site. */
	uint8_t *result = NULL;
	int history_pos = -1;

	replaceMinibufferText(E.minibuf, "");

	/* Save editor buffer and switch to minibuffer */
	E.edbuf = E.buf;
	E.buf = E.minibuf;

	while (1) {
		/* Display prompt with minibuffer content */
		char *content = E.minibuf->numrows > 0 ?
					(char *)E.minibuf->row[0].chars :
					"";
		if (!E.minibuf->completion_state.preserve_message) {
			setStatusMessage("%s%s", prompt, content);
		}
		E.minibuf->completion_state.preserve_message = 0;

		refreshScreen();

		/* Position cursor on bottom line.  cursorBottomLine
		 * expects a display column; E.minibuf->cx is a byte
		 * index, so convert (a CJK character is 3 bytes but 2
		 * columns; passing bytes drifts the cursor right of
		 * the text). */
		int prompt_width = stringWidth((const uint8_t *)prompt);
		int content_cols = 0;
		if (E.minibuf->numrows > 0)
			content_cols = charsToDisplayColumn(&E.minibuf->row[0],
							    E.minibuf->cx);
		cursorBottomLine(prompt_width + content_cols + 1);

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
		case '\r': {
			if (E.minibuf->numrows > 0 &&
			    E.minibuf->row[0].size > 0) {
				char *current_text =
					(char *)E.minibuf->row[0].chars;

				/* Determine the effective path: if a completion is selected,
				 * use that; otherwise use the minibuffer text. */
				struct completion_state *cs =
					&E.minibuf->completion_state;
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
							E.minibuf->row[0].size =
								len - 1;
						}
					}
				}

				/* Return the effective path */
				result = (uint8_t *)xstrdup(effective_path);
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
			if (E.minibuf->completion_state.matches &&
			    E.minibuf->completion_state.n_matches > 0) {
				cycleCompletion(E.minibuf, down ? 1 : -1);
				break;
			}

			struct history *hist = NULL;
			char *history_str = NULL;

			switch (t) {
			case PROMPT_FILES:
			case PROMPT_DIR:
				hist = &E.file_history;
				break;
			case PROMPT_COMMAND:
				hist = &E.command_history;
				break;
			case PROMPT_BASIC:
			case PROMPT_BUFFER:
				hist = &E.shell_history;
				break;
			case PROMPT_SEARCH:
				hist = &E.search_history;
				break;
			}

			if (hist && hist->count > 0) {
				if (!down) {
					if (history_pos == -1) {
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
				} else {
					replaceMinibufferText(E.minibuf, "");
				}
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
			    E.minibuf->completion_state.last_completed_text !=
				    NULL) {
				resetCompletionState(
					&E.minibuf->completion_state);
			}

			/* Dispatch */
			if (c >= ' ' && c < KEY_ARROW_LEFT)
				E.self_insert_key = c;
			if (cmd_peek != CMD_NONE)
				processKeypress(cmd_peek);

			/* Ensure single line */
			if (E.minibuf->numrows > 1) {
				/* Join all rows into first row */
				int total_len = 0;
				for (int i = 0; i < E.minibuf->numrows; i++) {
					total_len += E.minibuf->row[i].size;
				}

				char *joined = xmalloc(total_len + 1);
				joined[0] = 0;
				for (int i = 0; i < E.minibuf->numrows; i++) {
					if (E.minibuf->row[i].chars) {
						strncat(joined,
							(char *)E.minibuf
								->row[i]
								.chars,
							E.minibuf->row[i].size);
					}
				}

				replaceMinibufferText(E.minibuf, joined);
				free(joined);
			}
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
	if (result && strlen((char *)result) > 0) {
		struct history *hist = NULL;
		switch (t) {
		case PROMPT_FILES:
		case PROMPT_DIR:
			hist = &E.file_history;
			break;
		case PROMPT_COMMAND:
			hist = &E.command_history;
			break;
		case PROMPT_BASIC:
		case PROMPT_BUFFER:
			hist = &E.shell_history;
			break;
		case PROMPT_SEARCH:
			hist = &E.search_history;
			break;
		}
		if (hist) {
			addHistory(hist, (char *)result);
		}
	}

	closeCompletionsBuffer();

	E.buf = E.edbuf;

	clearStatusMessage();

	return result;
}
