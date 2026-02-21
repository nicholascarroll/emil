#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "prompt.h"
#include "emil.h"
#include "message.h"
#include "terminal.h"
#include "display.h"
#include "keymap.h"
#include "unicode.h"
#include "edit.h"
#include "buffer.h"
#include "util.h"
#include "completion.h"
#include "history.h"

extern struct editorConfig E;

uint8_t *editorPrompt(struct editorBuffer *bufr, uint8_t *prompt,
		      enum promptType t,
		      void (*callback)(struct editorBuffer *, uint8_t *, int)) {
	uint8_t *result = NULL;
	int history_pos = -1;

	while (E.minibuf->numrows > 0) {
		editorDelRow(E.minibuf, 0);
	}
	editorInsertRow(E.minibuf, 0, "", 0);
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
			editorSetStatusMessage((char *)prompt, content);
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
		int c = editorReadKey();
		editorRecordKey(c);

		int callback_key = c;

		/* Handle special minibuffer keys */
		switch (c) {
		case '\r': {
			if (E.minibuf->numrows > 0 &&
			    E.minibuf->row[0].size > 0) {
				char *current_text =
					(char *)E.minibuf->row[0].chars;
				struct stat st;

				/* Check if this is a file prompt and the path exists */
				if (t == PROMPT_FILES &&
				    stat(current_text, &st) == 0) {
					if (S_ISDIR(st.st_mode)) {
						/* User hit Enter on a directory. 
                 * Treat it like a Tab press instead. */
						int len = strlen(current_text);
						if (len > 0 &&
						    current_text[len - 1] !=
							    '/') {
							/* Move cursor to end before inserting slash */
							E.minibuf->cx = len;
							editorInsertChar(
								E.minibuf, '/',
								1);
						}

						handleMinibufferCompletion(
							E.minibuf, t);
						break; /* Do NOT return; keep the user in the prompt */
					}
				}

				/* PROMPT_DIR: strip trailing slash before returning */
				if (t == PROMPT_DIR) {
					int len = strlen(current_text);
					if (len > 1 &&
					    current_text[len - 1] == '/') {
						current_text[len - 1] = '\0';
						E.minibuf->row[0].size =
							len - 1;
					}
				}

				/* If it's a file, or it doesn't exist (new file), return it */
				/* If a completion is selected, return its full
				 * path rather than the basename shown in the
				 * minibuffer. */
				struct completion_state *cs =
					&E.minibuf->completion_state;
				if (cs->matches && cs->selected >= 0 &&
				    cs->selected < cs->n_matches) {
					result = (uint8_t *)xstrdup(
						cs->matches[cs->selected]);
				} else {
					result = (uint8_t *)xstrdup(
						current_text);
				}
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
			handleMinibufferCompletion(E.minibuf, t);
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
						editorDelRow(E.minibuf, 0);
					}
					editorInsertRow(E.minibuf, 0,
							last_search,
							strlen(last_search));
					E.minibuf->cx = strlen(last_search);
					E.minibuf->cy = 0;
				} else {
					editorSetStatusMessage(
						"[No previous search]");
				}
			}
			break;

		case ARROW_UP:
		case META_P:
		case ARROW_DOWN:
		case META_N: {
			/* If completions are visible, cycle selection
			 * instead of history. */
			if (E.minibuf->completion_state.matches &&
			    E.minibuf->completion_state.n_matches > 0) {
				cycleCompletion(E.minibuf,
						c == META_P ? 1 : -1);
				cycleCompletion(E.minibuf,
						c == META_N || c == ARROW_DOWN ?
							1 :
							-1);
				break;
			}

			struct editorHistory *hist = NULL;
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
				hist = &E.shell_history;
				break;
			case PROMPT_SEARCH:
				hist = &E.search_history;
				break;
			}

			if (hist && hist->count > 0) {
				if (c == META_P || c == ARROW_UP) {
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
							editorDelRow(E.minibuf,
								     0);
						}
						editorInsertRow(
							E.minibuf, 0,
							history_str,
							strlen(history_str));
						E.minibuf->cx =
							strlen(history_str);
						E.minibuf->cy = 0;
					}
				} else {
					while (E.minibuf->numrows > 0) {
						editorDelRow(E.minibuf, 0);
					}
					editorInsertRow(E.minibuf, 0, "", 0);
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

			editorProcessKeypress(c);

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
					editorDelRow(E.minibuf, 0);
				}
				editorInsertRow(E.minibuf, 0, joined,
						strlen(joined));
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
		struct editorHistory *hist = NULL;
		switch (t) {
		case PROMPT_FILES:
		case PROMPT_DIR:
			hist = &E.file_history;
			break;
		case PROMPT_COMMAND:
			hist = &E.command_history;
			break;
		case PROMPT_BASIC:
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

	/* Destroy the completions buffer entirely */
	struct editorBuffer *comp_buf = NULL;
	for (struct editorBuffer *b = E.headbuf; b != NULL; b = b->next) {
		if (b->filename && strcmp(b->filename, "*Completions*") == 0) {
			comp_buf = b;
			break;
		}
	}
	if (comp_buf) {
		destroyBuffer(comp_buf);
	}

	E.buf = E.edbuf;

	editorSetStatusMessage("");

	return result;
}
