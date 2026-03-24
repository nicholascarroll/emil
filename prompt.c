#include "prompt.h"
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

uint8_t *editorPrompt(struct buffer *bufr, const uint8_t *prompt,
		      enum promptType t,
		      void (*callback)(struct buffer *, uint8_t *, int)) {
	uint8_t *result = NULL;
	int history_pos = -1;

	while (E.minibuf->numrows > 0) {
		delRow(E.minibuf, 0);
	}
	insertRow(E.minibuf, 0, "", 0);
	E.minibuf->cx = 0;
	E.minibuf->cy = 0;

	/* Save editor buffer and switch to minibuffer */
	E.edbuf = E.buf;
	E.buf = E.minibuf;

	while (1) {
		/* Display prompt with minibuffer content */
		char *content = E.minibuf->numrows > 0 ?
					(char *)E.minibuf->row[0].chars :
					"";
		if (!E.minibuf->completion_state.preserve_message) {
			setStatusMessage((char *)prompt, content);
		}
		E.minibuf->completion_state.preserve_message = 0;

		/* Grow minibuffer if prompt + content exceeds one line */
		int total_len = strlen(E.statusmsg);
		int needed = (total_len + E.screencols - 1) / E.screencols;
		if (needed < 1)
			needed = 1;
		if (needed > 5)
			needed = 5;
		if (needed != minibuffer_height) {
			minibuffer_height = needed;
			/* Force window height recalculation */
			for (int w = 0; w < E.nwindows; w++)
				E.windows[w]->height = 0;
		}

		refreshScreen();

		/* Position cursor on bottom line */
		int prompt_width = stringWidth(prompt) - 2;
		cursorBottomLine(prompt_width + E.minibuf->cx + 1);

		/* Read key */
		int c = readKey();
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
				if (t == PROMPT_FILES &&
				    stat(effective_path, &st) == 0) {
					if (S_ISDIR(st.st_mode)) {
						/* User hit Enter on a directory.
						 * Replace minibuffer with the directory path,
						 * append / if needed, and trigger completion. */
						int elen =
							strlen(effective_path);

						/* Replace minibuffer content with effective path */
						if (effective_path !=
						    current_text) {
							while (E.minibuf->numrows >
							       0)
								delRow(E.minibuf,
								       0);
							insertRow(
								E.minibuf, 0,
								effective_path,
								elen);
							E.minibuf->cx = elen;
							E.minibuf->cy = 0;
							resetCompletionState(
								cs);
						}

						if (elen > 0 &&
						    effective_path[elen - 1] !=
							    '/') {
							E.minibuf->cx =
								E.minibuf
									->row[0]
									.size;
							insertChar(E.minibuf,
								   '/', 1);
						}

						handleMinibufferCompletion(
							E.minibuf, t);
						break; /* Do NOT return; keep the user in the prompt */
					}
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
			    t == PROMPT_COMMAND || t == PROMPT_BUFFER ||
			    t == PROMPT_SEARCH) {
				handleMinibufferCompletion(E.minibuf, t);
			} else {
				insertChar(E.minibuf, '\t', 1);
			}
			break;

		case CTRL('s'):
			/* C-s C-s: populate empty search with last search */
			if (t == PROMPT_SEARCH && E.minibuf->numrows > 0 &&
			    E.minibuf->row[0].size == 0) {
				char *last_search = NULL;
				struct historyEntry *last_entry =
					getLastHistory(&E.search_history);
				if (last_entry)
					last_search = last_entry->str;
				if (last_search) {
					while (E.minibuf->numrows > 0) {
						delRow(E.minibuf, 0);
					}
					insertRow(E.minibuf, 0, last_search,
						  strlen(last_search));
					E.minibuf->cx = strlen(last_search);
					E.minibuf->cy = 0;
				} else {
					setStatusMessage(
						"[No previous search]");
				}
			}
			break;

		case CTRL('r'):
			/* C-r C-r: populate empty search with last search */
			if (t == PROMPT_SEARCH && E.minibuf->numrows > 0 &&
			    E.minibuf->row[0].size == 0) {
				char *last_search = NULL;
				struct historyEntry *last_entry =
					getLastHistory(&E.search_history);
				if (last_entry)
					last_search = last_entry->str;
				if (last_search) {
					while (E.minibuf->numrows > 0) {
						delRow(E.minibuf, 0);
					}
					insertRow(E.minibuf, 0, last_search,
						  strlen(last_search));
					E.minibuf->cx = strlen(last_search);
					E.minibuf->cy = 0;
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
			if (E.minibuf->completion_state.matches &&
			    E.minibuf->completion_state.n_matches > 0) {
				cycleCompletion(E.minibuf,
						c == KEY_META('p') ? 1 : -1);
				cycleCompletion(
					E.minibuf,
					c == KEY_META('n') ||
							c == KEY_ARROW_DOWN ?
						1 :
						-1);
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
				if (c == KEY_META('p') || c == KEY_ARROW_UP) {
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
						while (E.minibuf->numrows > 0) {
							delRow(E.minibuf, 0);
						}
						insertRow(E.minibuf, 0,
							  history_str,
							  strlen(history_str));
						E.minibuf->cx =
							strlen(history_str);
						E.minibuf->cy = 0;
					}
				} else {
					while (E.minibuf->numrows > 0) {
						delRow(E.minibuf, 0);
					}
					insertRow(E.minibuf, 0, "", 0);
					E.minibuf->cx = 0;
					E.minibuf->cy = 0;
				}
			}
			break;
		}

		default:
			if (E.minibuf->completion_state.last_completed_text !=
			    NULL) {
				resetCompletionState(
					&E.minibuf->completion_state);
			}

			/* Resolve key → command before dispatching */
			if (c >= ' ' && c < KEY_ARROW_LEFT)
				E.self_insert_key = c;
			{
				int cmd = resolveBinding(c);
				if (cmd != CMD_NONE)
					processKeypress(cmd);
			}

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

				while (E.minibuf->numrows > 0) {
					delRow(E.minibuf, 0);
				}
				insertRow(E.minibuf, 0, joined, strlen(joined));
				E.minibuf->cx = strlen(joined);
				E.minibuf->cy = 0;
				free(joined);
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
	/* Restore minibuffer to single line */
	if (minibuffer_height != 1) {
		minibuffer_height = 1;
		for (int w = 0; w < E.nwindows; w++)
			E.windows[w]->height = 0;
	}

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

	setStatusMessage("");

	return result;
}
