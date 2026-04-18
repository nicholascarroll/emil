#include "keymap.h"
#include "adjust.h"
#include "buffer.h"
#include "clang.h"
#include "completion.h"
#include "display.h"
#include "edit.h"
#include "emil.h"
#include "fileio.h"
#include "find.h"
#include "message.h"
#include "pipe.h"
#include "prompt.h"
#include "region.h"
#include "register.h"
#include "terminal.h"
#include "transform.h"
#include "undo.h"
#include "unicode.h"
#include "unused.h"
#include "util.h"
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

extern struct config E;

/* Helper functions for state machine */
void showPrefix(const char *prefix) {
	setStatusMessage("%s", prefix);
}

// Forward declarations for command functions

static int compare_commands(const void *a, const void *b) {
	return strcmp(((struct command *)a)->key, ((struct command *)b)->key);
}

void setupCommands(void) {
	static struct command commands[] = {
		{ "capitalize-region", capitalizeRegion },
		//		{ "indent-spaces", indentSpaces },
		//		{ "indent-tabs", indentTabs },
		{ "insert-file", insertFile },
		{ "cd", changeDirectory },
		{ "diff-buffer-with-file", diffBufferWithFile },
		{ "isearch-forward-regexp", regexFindWrapper },
		{ "query-replace", queryReplace },
		{ "replace-regexp", replaceRegex },
		{ "replace-string", replaceString },
		{ "revert-buffer", revert },
		{ "visual-line-mode", toggleVisualLineMode },
		{ "version", editorVersion },
		{ "view-register", viewRegister },
#ifdef EMIL_DEBUG_UNDO
		{ "debug-unpair", debugUnpair },
#endif
	};

	E.cmd = commands;
	E.cmd_count = sizeof(commands) / sizeof(commands[0]);

	// Sort the commands array
	qsort(E.cmd, E.cmd_count, sizeof(struct command), compare_commands);
}

void runCommand(char *cmd) {
	for (int i = 0; cmd[i]; i++) {
		uint8_t c = cmd[i];
		if ('A' <= c && c <= 'Z') {
			c |= 0x60;
		} else if (c == ' ') {
			c = '-';
		}
		cmd[i] = c;
	}

	struct command key = { cmd, NULL };
	struct command *found = bsearch(&key, E.cmd, E.cmd_count,
					sizeof(struct command),
					compare_commands);

	if (found) {
		found->cmd();
	} else {
		setStatusMessage(msg_no_command);
	}
}

/*** editor operations ***/

void recordKey(int c) {
	if (E.recording) {
		E.macro.keys[E.macro.nkeys++] = c;
		if (E.macro.nkeys >= E.macro.skeys) {
			E.macro.skeys *= 2;
			E.macro.keys = xrealloc(E.macro.keys,
						E.macro.skeys * sizeof(int));
		}
		if (c == KEY_UNICODE) {
			for (int i = 0; i < E.nunicode; i++) {
				E.macro.keys[E.macro.nkeys++] = E.unicode[i];
				if (E.macro.nkeys >= E.macro.skeys) {
					E.macro.skeys *= 2;
					E.macro.keys = xrealloc(
						E.macro.keys,
						E.macro.skeys * sizeof(int));
				}
			}
		}
	}
}

/*** input ***/

/*
 * Resolve a Meta key combination into a command token.
 * The character is the raw byte after ESC (already extracted
 * by readKey and encoded as KEY_META(ch)).
 */
static int resolveMetaBinding(int ch) {
	/* Lowercase letters and their uppercase equivalents */
	switch (ch) {
	case 'b':
	case 'B':
		return CMD_BACKWARD_WORD;
	case 'c':
	case 'C':
		return CMD_CAPCASE_WORD;
	case 'd':
	case 'D':
		return CMD_DELETE_WORD;
	case 'f':
	case 'F':
		return CMD_FORWARD_WORD;
	case 'g':
	case 'G':
		return CMD_GOTO_LINE;
	case 'h':
		return CMD_MARK_PARA;
	case 'k':
		return CMD_KILL_PARA;
	case 'l':
	case 'L':
		return CMD_DOWNCASE_WORD;
	case 't':
	case 'T':
		return CMD_TRANSPOSE_WORDS;
	case 'u':
	case 'U':
		return CMD_UPCASE_WORD;
	case 'v':
	case 'V':
		return CMD_PAGE_UP;
	case 'w':
	case 'W':
		return CMD_COPY;
	case 'x':
	case 'X':
		return CMD_EXEC_CMD;
	case 'y':
	case 'Y':
		return CMD_YANK_POP;
	case 'a':
		return CMD_SENTENCE_BACKWARD;
	case 'e':
		return CMD_SENTENCE_FORWARD;
	case 'n':
		return CMD_SCROLL_DOWN;
	case 'p':
		return CMD_SCROLL_UP;
	case 'z':
		return CMD_ZAP_TO_CHAR;
	/* Punctuation and symbols */
	case '<':
		return CMD_BEG_OF_FILE;
	case '>':
		return CMD_END_OF_FILE;
	case '|':
		return CMD_PIPE_CMD;
	case '!':
		return CMD_SHELL_CMD;
	case '.':
		return CMD_CTAGS_JUMP;
	case ',':
		return CMD_CTAGS_BACK;
	case '`':
		return CMD_TOGGLE_HEADER_BODY;
	case '%':
		return CMD_QUERY_REPLACE;
	case '?':
		return CMD_CUSTOM_INFO;
	case '{':
		return CMD_BACKWARD_PARA;
	case '}':
		return CMD_FORWARD_PARA;
	case 127:
		return CMD_BACKSPACE_WORD;
	/* C-M- (control+meta) combinations: ESC followed by a control char */
	case CTRL('f'):
		return CMD_FORWARD_SEXP;
	case CTRL('b'):
		return CMD_BACKWARD_SEXP;
	case CTRL('k'):
		return CMD_KILL_SEXP;
	case CTRL('s'):
		return CMD_REGEX_SEARCH_FORWARD;
	case CTRL('r'):
		return CMD_REGEX_SEARCH_BACKWARD;
	default:
		return CMD_NONE;
	}
}

/*
 * Resolve a key token into a command token.
 *
 * Manages prefix state (C-x, C-x r), accumulates universal argument,
 * and translates key sequences into editor commands. Returns CMD_NONE
 * if the key sequence is incomplete (need more keys).
 */
int resolveBinding(int key) {
	static enum PrefixState prefix = PREFIX_NONE;

	/* Handle prefix state transitions */
	if (key == CTRL('x') && prefix == PREFIX_NONE) {
		prefix = PREFIX_CTRL_X;
		showPrefix("C-x ");
		return CMD_NONE;
	}

	if (key == CTRL('g') && prefix != PREFIX_NONE) {
		prefix = PREFIX_NONE;
		setStatusMessage("");
		return CMD_NONE;
	}

	/* C-x prefixed commands */
	if (prefix == PREFIX_CTRL_X) {
		prefix = PREFIX_NONE;

		switch (key) {
		case CTRL('c'):
			return CMD_QUIT;
		case CTRL('s'):
			return CMD_SAVE;
		case CTRL('w'):
			return CMD_SAVE_AS;
		case CTRL('q'):
			return CMD_TOGGLE_READ_ONLY;
		case CTRL('f'):
			return CMD_FIND_FILE;
		case CTRL('r'):
			return CMD_FIND_FILE_READ_ONLY;
		case CTRL('_'):
			return CMD_REDO;
		case CTRL('x'):
			return CMD_SWAP_MARK;
		case CTRL('t'):
			return CMD_TRANSPOSE_SENTENCES;
		case 'b':
		case 'B':
		case CTRL('b'):
			return CMD_SWITCH_BUFFER;
		case 'h':
			return CMD_MARK_BUFFER;
		case 'i':
			return CMD_INSERT_FILE;
		case 'o':
		case 'O':
			return CMD_OTHER_WINDOW;
		case '0':
			return CMD_DESTROY_WINDOW;
		case '1':
			return CMD_DESTROY_OTHER_WINDOWS;
		case '2':
			return CMD_CREATE_WINDOW;
		case 'k':
			return CMD_KILL_BUFFER;
		case '(':
			return CMD_MACRO_RECORD;
		case ')':
			return CMD_MACRO_END;
		case 'e':
		case 'E':
			return CMD_MACRO_EXEC;
		case 'z':
		case 'Z':
			return CMD_SUSPEND;
		case CTRL('z'):
			return CMD_SHELL_DRAWER;
		case 'u':
		case 'U':
		case CTRL('u'):
			return CMD_UPCASE_REGION;
		case 'l':
		case 'L':
		case CTRL('l'):
			return CMD_DOWNCASE_REGION;
		case KEY_BACKSPACE:
			return CMD_KILL_LINE_BACKWARDS;
		case '=':
			return CMD_WHAT_CURSOR;
		case 'x':
			/* C-x x sub-prefix — read another key */
			{
				int nextkey = readKey();
				if (nextkey == 't') {
					return CMD_VISUAL_LINE_MODE;
				} else {
					setStatusMessage(msg_unknown_cx_x,
							 nextkey);
					return CMD_NONE;
				}
			}
		case 'r':
		case 'R':
			prefix = PREFIX_CTRL_X_R;
			showPrefix("C-x r");
			return CMD_NONE;
		case KEY_ARROW_LEFT:
			return CMD_PREV_BUFFER;
		case KEY_ARROW_RIGHT:
			return CMD_NEXT_BUFFER;
		case ' ':
			if (!E.buf->mark_active)
				setMark();
			toggleRectangleMode();
			return CMD_NONE;
		default:
			if (key < ' ') {
				setStatusMessage(msg_unknown_cx_ctrl,
						 key + '`');
			} else {
				setStatusMessage(msg_unknown_cx, key);
			}
			return CMD_NONE;
		}
	}

	/* C-x r prefixed commands */
	if (prefix == PREFIX_CTRL_X_R) {
		prefix = PREFIX_NONE;

		switch (key) {
		case '\x1b': {
			int nextkey = readKey();
			if (IS_META_KEY(nextkey)) {
				int mch = META_CHAR(nextkey);
				if (mch == 'W' || mch == 'w')
					return CMD_COPY_RECT;
			}
			/* Raw ESC handling for readKey returning
			 * key tokens directly */
			if (nextkey == 'W' || nextkey == 'w')
				return CMD_COPY_RECT;
			setStatusMessage("Unknown command C-x r ESC %c",
					 nextkey);
			return CMD_NONE;
		}
		case 'j':
		case 'J':
			return CMD_JUMP_REGISTER;
		case ' ':
			return CMD_POINT_REGISTER;
		case 'r':
		case 'R':
			return CMD_RECT_REGISTER;
		case 's':
		case 'S':
			if (E.buf->rectangle_mode)
				return CMD_RECT_REGISTER;
			else
				return CMD_REGION_REGISTER;
		case 't':
		case 'T':
			return CMD_STRING_RECT;
		case '+':
			return CMD_INC_REGISTER;
		case 'i':
		case 'I':
			return CMD_INSERT_REGISTER;
		case 'k':
		case 'K':
		case CTRL('W'):
			return CMD_KILL_RECT;
		case 'v':
		case 'V':
			return CMD_VIEW_REGISTER;
		case 'y':
		case 'Y':
			return CMD_YANK_RECT;
		default:
			if (key < ' ') {
				setStatusMessage("Unknown command C-x r C-%c",
						 key + '`');
			} else {
				setStatusMessage("Unknown command C-x r %c",
						 key);
			}
			return CMD_NONE;
		}
	}

	prefix = PREFIX_NONE;

	/* Meta key combinations */
	if (IS_META_KEY(key)) {
		int cmd = resolveMetaBinding(META_CHAR(key));
		if (cmd != CMD_NONE)
			return cmd;
		/* Unknown Meta combination — already handled by terminal
		 * layer's ESC_UNKNOWN path with status message */
		return CMD_NONE;
	}

	/* Alt digits */
	if (key >= KEY_ALT_0 && key <= KEY_ALT_9)
		return key; /* Passed through, handled by processKeypress */

	/* Physical key tokens → commands */
	switch (key) {
	case KEY_ARROW_LEFT:
		return CMD_BACKWARD_CHAR;
	case KEY_ARROW_RIGHT:
		return CMD_FORWARD_CHAR;
	case KEY_ARROW_UP:
		return CMD_PREV_LINE;
	case KEY_ARROW_DOWN:
		return CMD_NEXT_LINE;
	case KEY_HOME:
		return CMD_HOME;
	case KEY_END:
		return CMD_END;
	case KEY_DEL:
		return CMD_DELETE;
	case KEY_PAGE_UP:
		return CMD_PAGE_UP;
	case KEY_PAGE_DOWN:
		return CMD_PAGE_DOWN;
	case KEY_BACKTAB:
		return CMD_UNINDENT;
	case KEY_BACKSPACE:
		return CMD_BACKSPACE;
	case KEY_UNICODE:
		return CMD_UNICODE;
	case KEY_UNICODE_ERROR:
		return CMD_UNICODE_ERROR;
	}

	/* Ctrl key combinations */
	switch (key) {
	case CTRL('a'):
		return CMD_HOME;
	case CTRL('b'):
		return CMD_BACKWARD_CHAR;
	case CTRL('d'):
		return CMD_DELETE;
	case CTRL('e'):
		return CMD_END;
	case CTRL('f'):
		return CMD_FORWARD_CHAR;
	case CTRL('g'):
		return CMD_CANCEL;
	case CTRL('h'):
		return CMD_HELP;
	case CTRL('j'):
		return CMD_NEWLINE_INDENT;
	case CTRL('k'):
		return CMD_KILL_LINE;
	case CTRL('l'):
		return CMD_RECENTER;
	case CTRL('n'):
		return CMD_NEXT_LINE;
	case CTRL('o'):
		return CMD_OPEN_LINE;
	case CTRL('p'):
		return CMD_PREV_LINE;
	case CTRL('q'):
		return CMD_QUOTED_INSERT;
	case CTRL('r'):
		return CMD_REVERSE_ISEARCH;
	case CTRL('s'):
		return CMD_ISEARCH;
	case CTRL('t'):
		return CMD_TRANSPOSE_CHARS;
	case CTRL('u'):
		return CMD_UNIVERSAL_ARG;
	case CTRL('v'):
		return CMD_PAGE_DOWN;
	case CTRL('w'):
		return CMD_KILL_REGION;
	case CTRL('y'):
		return CMD_YANK;
	case CTRL('z'):
		return CMD_SUSPEND;
	case CTRL('_'):
		return CMD_UNDO;
	case CTRL('@'):
		return CMD_SET_MARK;
	case CTRL('C'):
		return CMD_COPY_CLIPBOARD;
	case '\r':
		return CMD_NEWLINE;
	case '\t':
		return CMD_TAB;
	case 033:
		return CMD_NONE; /* Bare ESC — already handled */
	}

	/* Printable characters — check for digit accumulation after C-u */
	if (key >= ' ' && key < KEY_ARROW_LEFT) {
		if (E.uarg && key >= '0' && key <= '9') {
			if (E.uarg == 4) {
				E.uarg = key - '0';
			} else {
				E.uarg = E.uarg * 10 + (key - '0');
			}
			setStatusMessage("C-u %d", E.uarg);
			return CMD_NONE;
		}
		return CMD_SELF_INSERT;
	}

	return CMD_NONE;
}

/*
 * Grouped command dispatch functions.
 *
 * Each function handles a category of commands. Returns 1 if the key
 * was handled, 0 otherwise. The caller tries each group in order.
 */

/* Movement */
static int dispatchMove(int c, int uarg, struct window *win) {
	switch (c) {
	case CMD_BACKWARD_CHAR:
		moveCursor(KEY_ARROW_LEFT, uarg);
		return 1;
	case CMD_FORWARD_CHAR:
		moveCursor(KEY_ARROW_RIGHT, uarg);
		return 1;
	case CMD_PREV_LINE:
		moveCursor(KEY_ARROW_UP, uarg);
		return 1;
	case CMD_NEXT_LINE:
		moveCursor(KEY_ARROW_DOWN, uarg);
		return 1;
	case CMD_PAGE_UP:
		pageUp(uarg);
		return 1;
	case CMD_PAGE_DOWN:
		pageDown(uarg);
		return 1;
	case CMD_SCROLL_UP:
		scrollLineUp(uarg);
		return 1;
	case CMD_SCROLL_DOWN:
		scrollLineDown(uarg);
		return 1;
	case CMD_BEG_OF_FILE:
		setMarkSilent();
		E.buf->cy = 0;
		E.buf->cx = 0;
		return 1;
	case CMD_END_OF_FILE:
		setMarkSilent();
		E.buf->cy = E.buf->numrows;
		E.buf->cx = 0;
		return 1;
	case CMD_HOME:
		beginningOfLine();
		return 1;
	case CMD_END:
		endOfLine(uarg);
		return 1;
	case CMD_FORWARD_WORD:
		forwardWord(uarg);
		return 1;
	case CMD_BACKWARD_WORD:
		backWord(uarg);
		return 1;
	case CMD_FORWARD_PARA:
		forwardPara(uarg);
		return 1;
	case CMD_BACKWARD_PARA:
		backPara(uarg);
		return 1;
	case CMD_FORWARD_SEXP:
		forwardSexp(uarg);
		return 1;
	case CMD_BACKWARD_SEXP:
		backwardSexp(uarg);
		return 1;
	case CMD_SENTENCE_FORWARD:
		forwardSentence(uarg);
		return 1;
	case CMD_SENTENCE_BACKWARD:
		backwardSentence(uarg);
		return 1;
	case CMD_RECENTER:
		recenter(win);
		return 1;
	case CMD_GOTO_LINE:
		gotoLine();
		return 1;
	default:
		return 0;
	}
}

/* Editing */
static int dispatchEdit(int c, int uarg) {
	switch (c) {
	case CMD_NEWLINE:
		insertNewline(uarg);
		return 1;
	case CMD_BACKSPACE:
		backSpace(uarg);
		return 1;
	case CMD_HELP:
		help();
		return 1;
	case CMD_DELETE:
		delChar(uarg);
		return 1;
	case CMD_UNICODE_ERROR:
		setStatusMessage(msg_invalid_utf8);
		return 1;
	case CMD_UNICODE:
		insertUnicode(uarg);
		return 1;
	case CMD_KILL_LINE:
		killLine(uarg);
		return 1;
	case CMD_NEWLINE_INDENT:
		insertNewlineAndIndent(uarg);
		return 1;
	case CMD_OPEN_LINE:
		openLine(uarg);
		return 1;
	case CMD_QUOTED_INSERT: {
		int key = readKey();
		if (key == KEY_UNICODE) {
			int count = uarg ? uarg : 1;
			insertUnicode(count);
		} else if (key != KEY_UNICODE_ERROR && key < KEY_ARROW_LEFT) {
			int count = uarg ? uarg : 1;
			undoSelfInsert(key, count);
			insertChar(E.buf, key, count);
			if (count > 1)
				adjustAllPoints(E.buf, E.buf->cx - count,
						E.buf->cy, E.buf->cx, E.buf->cy,
						0);
		} else {
			setStatusMessage(msg_invalid_utf8);
		}
	}
		return 1;
	case CMD_BACKSPACE_WORD:
		backspaceWord(uarg);
		return 1;
	case CMD_DELETE_WORD:
		deleteWord(uarg);
		return 1;
	case CMD_UPCASE_WORD:
		upcaseWord(uarg);
		return 1;
	case CMD_DOWNCASE_WORD:
		downcaseWord(uarg);
		return 1;
	case CMD_CAPCASE_WORD:
		capitalCaseWord(uarg);
		return 1;
	case CMD_TRANSPOSE_WORDS:
		transposeWords();
		return 1;
	case CMD_TRANSPOSE_CHARS:
		if (E.buf->rectangle_mode && !markInvalidSilent())
			stringRectangle();
		else
			transposeChars();
		return 1;
	case CMD_TRANSPOSE_SENTENCES:
		transposeSentences();
		return 1;
	case CMD_ZAP_TO_CHAR:
		zapToChar();
		return 1;
	case CMD_KILL_SEXP:
		killSexp(uarg);
		return 1;
	case CMD_KILL_PARA:
		killParagraph(uarg);
		return 1;
	case CMD_MARK_PARA:
		markParagraph();
		return 1;
	case CMD_UNINDENT:
		unindent(uarg);
		return 1;
	default:
		return 0;
	}
}

/* Window management */
static int dispatchWindow(int c) {
	switch (c) {
	case CMD_OTHER_WINDOW:
		if (E.buf == E.minibuf) {
			setStatusMessage(
				"Command attempted to use minibuffer while in minibuffer");
		} else {
			switchWindow();
		}
		return 1;
	case CMD_CREATE_WINDOW:
		if (E.buf == E.minibuf) {
			setStatusMessage(
				"Command attempted to use minibuffer while in minibuffer");
		} else {
			createWindow();
		}
		return 1;
	case CMD_DESTROY_WINDOW:
		if (E.buf == E.minibuf) {
			setStatusMessage(
				"Command attempted to use minibuffer while in minibuffer");
		} else {
			destroyWindow(windowFocusedIdx());
		}
		return 1;
	case CMD_DESTROY_OTHER_WINDOWS:
		if (E.buf == E.minibuf) {
			setStatusMessage(
				"Command attempted to use minibuffer while in minibuffer");
		} else {
			destroyOtherWindows();
		}
		return 1;
	default:
		return 0;
	}
}

/* Buffers and files */
static int dispatchBuffer(int c, int uarg) {
	switch (c) {
	case CMD_QUIT:
		quit();
		return 1;
	case CMD_SAVE:
		save();
		return 1;
	case CMD_SAVE_AS:
		saveAs();
		return 1;
	case CMD_SWITCH_BUFFER:
		switchToNamedBuffer();
		return 1;
	case CMD_NEXT_BUFFER:
		nextBuffer();
		return 1;
	case CMD_PREV_BUFFER:
		previousBuffer();
		return 1;
	case CMD_MARK_BUFFER:
		markBuffer();
		return 1;
	case CMD_KILL_BUFFER:
		killBuffer();
		return 1;
	case CMD_INSERT_FILE:
		insertFile();
		return 1;
	case CMD_FIND_FILE:
		findFile(0);
		return 1;
	case CMD_FIND_FILE_READ_ONLY:
		findFile(1);
		return 1;
	case CMD_TOGGLE_READ_ONLY:
		E.buf->read_only = !E.buf->read_only;
		setStatusMessage(E.buf->read_only ? msg_read_only :
						    "Buffer set to writable");
		return 1;
	case CMD_REDO:
		doRedo(E.buf, uarg);
		if (E.buf->redo != NULL) {
			setStatusMessage("Press C-_ or C-/ to redo again");
			E.micro = CMD_REDO;
		}
		return 1;
	default:
		return 0;
	}
}

/* Region, registers, and rectangles */
static int dispatchRegion(int c, int uarg) {
	switch (c) {
	case CMD_SET_MARK:
		if (uarg) {
			popMark();
		} else {
			setMark();
		}
		return 1;
	case CMD_SWAP_MARK:
		if (0 <= E.buf->markx &&
		    (0 <= E.buf->marky && E.buf->marky < E.buf->numrows)) {
			int swapx = E.buf->cx;
			int swapy = E.buf->cy;
			E.buf->cx = E.buf->markx;
			E.buf->cy = E.buf->marky;
			E.buf->markx = swapx;
			E.buf->marky = swapy;
			E.buf->mark_active = 1;
		}
		return 1;
	case CMD_CUT:
		if (E.buf->rectangle_mode)
			killRectangle();
		else
			killRegion();
		deactivateMark();
		return 1;
	case CMD_COPY:
		if (E.buf->rectangle_mode)
			copyRectangle();
		else
			copyRegion();
		deactivateMark();
		return 1;
	case CMD_COPY_CLIPBOARD:
		if (!E.buf->rectangle_mode) {
			copyRegion();
			deactivateMark();
			copyToClipboard(E.kill.str);
		} else {
			setStatusMessage(
				"Copying rectangle to OSC 52 not supported!");
		}
		return 1;
	case CMD_YANK:
		if (E.kill.is_rectangle)
			yankRectangle();
		else
			yank(uarg);
		return 1;
	case CMD_YANK_POP:
		yankPop();
		return 1;
	case CMD_KILL_REGION:
		if (!E.buf->rectangle_mode) {
			killRegion();
		} else {
			killRectangle();
		}
		deactivateMark();
		return 1;
	case CMD_UPCASE_REGION:
		transformRegion(transformerUpcase);
		return 1;
	case CMD_DOWNCASE_REGION:
		transformRegion(transformerDowncase);
		return 1;
	case CMD_JUMP_REGISTER:
		jumpToRegister();
		return 1;
	case CMD_POINT_REGISTER:
		pointToRegister();
		return 1;
	case CMD_REGION_REGISTER:
		regionToRegister();
		return 1;
	case CMD_INC_REGISTER:
		incrementRegister();
		return 1;
	case CMD_INSERT_REGISTER:
		insertRegister();
		return 1;
	case CMD_VIEW_REGISTER:
		viewRegister();
		return 1;
	case CMD_STRING_RECT:
		stringRectangle();
		return 1;
	case CMD_COPY_RECT:
		copyRectangle();
		deactivateMark();
		return 1;
	case CMD_KILL_RECT:
		killRectangle();
		deactivateMark();
		return 1;
	case CMD_YANK_RECT:
		yankRectangle();
		return 1;
	case CMD_RECT_REGISTER:
		rectToRegister();
		return 1;
	default:
		return 0;
	}
}

/* Search and replace */
static int dispatchSearch(int c) {
	switch (c) {
	case CMD_ISEARCH:
		editorFind();
		return 1;
	case CMD_REVERSE_ISEARCH:
		reverseFind();
		return 1;
	case CMD_REGEX_SEARCH_FORWARD:
		regexFind();
		return 1;
	case CMD_REGEX_SEARCH_BACKWARD:
		backwardRegexFind();
		return 1;
	case CMD_QUERY_REPLACE:
		queryReplace();
		return 1;
	default:
		return 0;
	}
}

/* Macros */
static int dispatchMacro(int c, int uarg) {
	switch (c) {
	case CMD_MACRO_RECORD:
		if (!E.recording) {
			E.recording = 1;
			E.macro.nkeys = 0;
			E.macro.skeys = 32;
			if (E.macro.keys) {
				free(E.macro.keys);
			}
			E.macro.keys = xmalloc(E.macro.skeys * sizeof(int));
			setStatusMessage(msg_recording);
		} else {
			setStatusMessage(msg_already_recording);
		}
		return 1;
	case CMD_MACRO_END:
		if (E.recording) {
			E.recording = 0;
			setStatusMessage(msg_macro_recorded, E.macro.nkeys);
		} else {
			setStatusMessage(msg_not_recording);
		}
		return 1;
	case CMD_MACRO_EXEC:
		if (E.macro.nkeys > 0) {
			for (int i = 0; i < (uarg ? uarg : 1); i++) {
				execMacro(&E.macro);
			}
		} else {
			setStatusMessage(msg_no_macro);
		}
		return 1;
	default:
		return 0;
	}
}

/* Miscellaneous */
static int dispatchMisc(int c, int uarg) {
	switch (c) {
	case CMD_UNDO:
		doUndo(E.buf, uarg);
		return 1;
	case CMD_SUSPEND:
		raise(SIGTSTP);
		return 1;
	case CMD_SHELL_DRAWER:
		openShellDrawer();
		return 1;
	case CMD_CTAGS_JUMP:
		ctagsJump();
		return 1;
	case CMD_CTAGS_BACK:
		ctagsBack();
		return 1;
	case CMD_TOGGLE_HEADER_BODY:
		toggleHeaderBody();
		return 1;
	case CMD_VISUAL_LINE_MODE:
		toggleVisualLineMode();
		return 1;
	case CMD_WHAT_CURSOR:
		whatCursor();
		return 1;
	case CMD_EXEC_CMD: {
		if (E.recording || E.playback) {
			setStatusMessage(msg_macro_blocked);
			return 1;
		}
		uint8_t *cmd =
			editorPrompt(E.buf, "cmd: %s", PROMPT_COMMAND, NULL);
		if (cmd != NULL) {
			runCommand(cmd);
			free(cmd);
		}
	}
		return 1;
	case CMD_CUSTOM_INFO: {
		int winIdx = windowFocusedIdx();
		struct window *w = E.windows[winIdx];
		struct buffer *buf = w->buf;

		setStatusMessage(
			"(buf->cx%d,cy%d) (win->scx%d,scy%d) win->height=%d "
			"screenrows=%d, rowoff=%d",
			buf->cx, buf->cy, w->scx, w->scy, w->height,
			E.screenrows, w->rowoff);
	}
		return 1;
	case CMD_KILL_LINE_BACKWARDS:
		killLineBackwards();
		return 1;
	case CMD_CANCEL:
		deactivateMark();
		setStatusMessage(msg_quit);
		return 1;
	case CMD_UNIVERSAL_ARG:
		/* Handled before dispatch chain */
		return 1;
	default:
		return 0;
	}
}

void processKeypress(int c) {
	/* Record is handled by the caller (main loop) at the key level.
	 * Commands arriving here are already resolved. */

	if (c != CMD_YANK && c != CMD_YANK_POP) {
		E.kill_ring_pos = -1;
	}

	int windowIdx = windowFocusedIdx();
	struct window *win = E.windows[windowIdx];

	if (E.micro) {
		if (E.micro == CMD_REDO && (c == CMD_UNDO)) {
			doRedo(E.buf, 1);
			return;
		} else {
			E.micro = 0;
		}
	} else {
		E.micro = 0;
	}

	if (c >= KEY_ALT_0 && c <= KEY_ALT_9) {
		if (!E.uarg) {
			E.uarg = 0;
		}
		E.uarg *= 10;
		E.uarg += c - KEY_ALT_0;
		setStatusMessage("uarg: %i", E.uarg);
		return;
	}

	/* Handle C-u (Universal Argument) */
	if (c == CMD_UNIVERSAL_ARG) {
		if (!E.uarg) {
			E.uarg = 4;
		} else {
			E.uarg *= 4;
		}
		setStatusMessage("C-u %d", E.uarg);
		return;
	}

	/* Handle pipe/shell before general dispatch */
	if (c == CMD_PIPE_CMD) {
		pipeCmd(1);
		E.uarg = 0;
		return;
	}
	if (c == CMD_SHELL_CMD) {
		pipeCmd(0);
		E.uarg = 0;
		return;
	}

	int uarg = E.uarg;

	/* Dispatch through grouped handlers */
	if (dispatchMove(c, uarg, win))
		goto done;
	if (dispatchEdit(c, uarg))
		goto done;
	if (dispatchWindow(c))
		goto done;
	if (dispatchBuffer(c, uarg))
		goto done;
	if (dispatchRegion(c, uarg))
		goto done;
	if (dispatchSearch(c))
		goto done;
	if (dispatchMacro(c, uarg))
		goto done;
	if (dispatchMisc(c, uarg))
		goto done;

	/* Self-insert */
	if (c == CMD_TAB || c == CMD_SELF_INSERT) {
		if (c == CMD_TAB && E.buf == E.minibuf)
			goto done;
		int ch = (c == CMD_TAB) ? '\t' : E.self_insert_key;
		int count = uarg ? uarg : 1;
		undoSelfInsert(ch, count);
		insertChar(E.buf, ch, count);
		if (count > 1)
			adjustAllPoints(E.buf, E.buf->cx - count, E.buf->cy,
					E.buf->cx, E.buf->cy, 0);
	}

done:
	E.uarg = 0;
	clampPositions(E.buf);
}

/*** init ***/

void execMacro(struct macro *macro) {
	const int MAX_MACRO_DEPTH = 100;
	if (E.macro_depth >= MAX_MACRO_DEPTH) {
		setStatusMessage(msg_macro_depth);
		return;
	}

	E.macro_depth++;

	struct macro tmp;
	tmp.keys = NULL;
	if (macro != &E.macro) {
		/* HACK: Annoyance here with readkey needs us to futz
		 * around with E.macro */
		memcpy(&tmp, &E.macro, sizeof(struct macro));
		memcpy(&E.macro, macro, sizeof(struct macro));
	}
	E.playback = 0;
	while (E.playback < E.macro.nkeys) {
		/* HACK: increment here, so that
		 * readkey sees playback != 0 */
		int key = E.macro.keys[E.playback++];
		if (key == KEY_UNICODE) {
			deserializeUnicode();
		}
		if (key >= ' ' && key < KEY_ARROW_LEFT)
			E.self_insert_key = key;
		int cmd = resolveBinding(key);
		if (cmd != CMD_NONE)
			processKeypress(cmd);
	}
	E.playback = 0;
	if (tmp.keys != NULL) {
		memcpy(&E.macro, &tmp, sizeof(struct macro));
	}

	E.macro_depth--;
}
