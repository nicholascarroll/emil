/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#ifndef EMIL_H
#define EMIL_H 1

#include "abuf.h"
#include "keymap.h"
#include <stdint.h>
#include <stdlib.h>
#include <termios.h>
#include <time.h>

/*** util ***/

#define EMIL_TAB_STOP 8

#define EMIL_MAX_FILE_SIZE ((size_t)1024 * 1024 * 1024)

#ifndef EMIL_VERSION
#define EMIL_VERSION "unknown"
#endif

#define ESC "\033"
#define CSI ESC "["
#define CRLF "\r\n"

/* Universal argument encoding.
 * UARG_REVERSE is the M-- reverse modifier; UARG_COUNT extracts a
 * repeat count, treating "no argument" and "reverse" alike as 1.
 *
 * The encoding relies on counts and the sentinel occupying disjoint
 * value ranges: counts are always >= 1, so the sign carries the
 * discriminant. */
#define UARG_REVERSE (-1)
#define UARG_COUNT(u) ((u) > 0 ? (u) : 1)
#define UARG_MAX 1000000

/* Suppress GCC's warn_unused_result where the return value is
 * intentionally discarded (e.g. best-effort write to stdout). */
#define IGNORE_RETURN(expr) \
	do {                \
		if (expr) { \
		}           \
	} while (0)
#define ISCTRL(c) ((0 < c && c < 0x20) || c == 0x7f)

/* A prompt's type selects its completion behaviour and its history ring */
enum promptType {
	PROMPT_PLAIN, /* no completion, no history */
	PROMPT_BUFFER,
	PROMPT_FILES,
	PROMPT_DIR,
	PROMPT_COMMAND,
	PROMPT_SEARCH,
	PROMPT_REPLACE, /* both halves of every replace command */
	PROMPT_SHELL,
	PROMPT_RECT,
};
/*** data ***/

/* Type policy:
 * Positions (cx, cy, markx, marky): int, signed for sentinels
 * Sizes (erow.size, abuf.len): int, NOT bounded by anything at runtime
 * Accumulations to malloc: size_t (e.g. rowsToString totlen)
 *
 * This used to say erow.size and abuf.len were bounded by
 * EMIL_MAX_FILE_SIZE.  They are not.  The load path bounds a *file* to
 * that size at admission (§3.21.1); nothing re-imposes it once editing
 * starts, and a buffer can grow without limit from there.  So any code
 * growing one of these values owns its own overflow guard -- see
 * dbuf_ensure(), undoEnsureData(), rowEnsureCap().  The claim was true
 * under the older EMIL_BYTES_BUDGET and was left behind when that
 * went.  */

typedef struct erow {
	int size;
	int charcap; /* bytes allocated (>= size + 1) */
	uint8_t *chars;
	int cached_width; /* display width in columns, or -1 if stale.
			   * INVARIANT (§4.10): any code that modifies
			   * row text must set this to -1.  It is now
			   * the only derived field on a row: the wrap
			   * count that sat beside it went with the
			   * screen-line cache (#108), and the width
			   * does not depend on the terminal's, so a
			   * resize does not stale it. */
} erow;

struct undo {
	struct undo *prev;
	int startx;
	int starty;
	int endx;
	int endy;
	int append;
	int nmerges; /* operations folded into this record so far */
	int datalen;
	int datasize;
	int delete;
	int paired;
	uint8_t *data;
};

struct completionState {
	char *last_completed_text;
	int completion_start_pos;
	int successive_tabs;
	int last_completion_count;
	int preserve_message;
	int selected;	/* Currently highlighted match index, -1 = none */
	char **matches; /* Copy of match list for M-n/M-p navigation */
	int n_matches;	/* Number of matches in the list */
};

struct completionResult {
	char **matches;
	int n_matches;
	char *common_prefix;
	int prefix_len;
};

#define MARK_RING_SIZE 8

struct markRingEntry {
	int cx;
	int cy;
};

struct buffer {
	int cx, cy;
	int markx, marky;
	int mark_active;
	struct markRingEntry mark_ring[MARK_RING_SIZE];
	int mark_ring_len; /* number of valid entries (0..MARK_RING_SIZE) */
	int mark_ring_idx; /* next slot to write (circular) */
	int numrows;
	int rowcap;
	int end;
	int dirty;
	int special_buffer;
	int word_wrap;
	int rectangle_mode;
	int read_only;
	/* 1 when read_only was imposed by us because another process
	 * held an advisory lock at open time, and NOT by a failed
	 * access(W_OK) or by the user's own C-x C-q.  Only a read-only
	 * flagged this way may be lifted when the lock goes away; the
	 * other two reasons are not ours to undo. */
	int read_only_by_lock;
	int lock_fd; /* fd holding advisory lock, or -1 */
	/* Only used for equality comparison with stat().st_mtime.
	 * Safe across the 2038 boundary.  Do NOT do arithmetic on
	 * this field. */
	time_t open_mtime;    /* st_mtime at open/save, 0 if unset */
	off_t open_size;      /* st_size at the same moment; only
	                       * meaningful when open_mtime != 0.
	                       * st_mtime is whole seconds, so a write
	                       * in the load's second is invisible to
	                       * it alone. */
	int external_mod;     /* 1 if file changed on disk since open/save */
	int lock_blocked_pid; /* PID holding the lock we couldn't acquire,
	                       * or -1 if held by unknown process, or 0
	                       * if we are not blocked */
	int internal_mod;
	erow *row;
	char *filename;
	char *display_name; /* Truncated name for status bar display */
	int min_name_len;   /* Min chars to show without colliding */
	uint8_t *query;
	uint8_t match;
	int match_len; /* bytes matched at (cx, cy) when match is set.
			* For a regex this differs from the pattern
			* length, so the highlight cannot be derived
			* from strlen(query).  0 = not set. */
	struct undo *undo;
	struct undo *redo;
	struct buffer *next;
	struct completionState completionState;
};

struct window {
	int focused;
	struct buffer *buf;
	int cx, cy; // Buffer cx,cy  (only updated when switching windows)
	int rowoff;
	int coloff;
	int height;
	int skip_sublines; /* sub-lines of rowoff row to skip (derived per frame) */
};

struct macro {
	int *keys;
	int nkeys;
	int skeys;
};

struct command {
	const char *key;
	void (*cmd)(void);
};

struct text {
	uint8_t *str;	  /* NUL-terminated data */
	int is_rectangle; /* 1 = rectangle data, 0 = plain text */
	int rect_width;	  /* column width (meaningful when is_rectangle) */
	int rect_height;  /* row count (meaningful when is_rectangle) */
};

static inline void clearText(struct text *t) {
	free(t->str);
	t->str = NULL;
	t->is_rectangle = 0;
	t->rect_width = 0;
	t->rect_height = 0;
}

enum registerType {
	REGISTER_NULL,
	REGISTER_POINT,
	REGISTER_TEXT,
};

struct point {
	int cx;
	int cy;
	struct buffer *buf;
};

struct editorRegister {
	enum registerType rtype;
	union {
		struct point point;
		struct text text;
	} data;
};

#define HISTORY_MAX_ENTRIES 100

struct historyEntry {
	char *str;
	int is_rectangle; /* kill ring only; zero for other histories */
	int rect_width;	  /* kill ring only; zero for other histories */
	int rect_height;  /* kill ring only; zero for other histories */
	struct historyEntry *prev;
	struct historyEntry *next;
};

struct history {
	struct historyEntry *head;
	struct historyEntry *tail;
	int count;
};

struct config {
	/* Active kill entry.
	 *
	 * Deliberately shared between the minibuffer and ordinary
	 * buffers, as in Emacs: a kill made while typing at a prompt
	 * lands in the same ring the file yanks from, which is what
	 * makes C-w at a prompt then C-y in the buffer work.  The kill
	 * ring, kill_ring_pos and the history rings below are therefore
	 * NOT saved and restored across a nested prompt -- unlike
	 * edbuf and prompt_type, which are per-prompt state.  A nested
	 * prompt moving the outer buffer's yank position is the
	 * intended behaviour, not a nesting bug. */
	struct text kill;
	int screenrows;
	int screencols;
	uint8_t unicode[4];
	int nunicode;
	char statusmsg[1024];
	char prefix_display[32]; /* Display prefix commands like C-u */

	/* Buffer management for minibuffer */
	struct buffer *edbuf;	/* Saved editor context */
	struct buffer *minibuf; /* Minibuffer object */
	/* Type of the prompt currently reading the minibuffer.
	 * Meaningful only while E.buf == E.minibuf; editorPrompt
	 * saves, sets and restores it, so nested prompts see their
	 * own type.  Lets keymap.c refuse a quoted newline in
	 * prompts whose consumers cannot honour one. */
	enum promptType prompt_type;

	int statusmsg_show;
	struct termios orig_termios;
	struct buffer *headbuf;
	struct buffer *buf; /* Current active buffer */
	int nwindows;
	struct window **windows;
	int recording;
	struct macro macro;
	int playback;
	/* The command that just ran, when it changes what the *next*
	 * keystroke means.  The only such case today is CMD_REDO:
	 * after a C-/ that leaves more to redo, a following C-_ or
	 * C-/ continues redoing rather than undoing (keymap.c's redo
	 * chain).  Every other key clears it, so it never outlives
	 * the keystroke after the one that set it.
	 *
	 * emil-findings.md §F6 lists this as vestigial.  It is not:
	 * keymap.c:1170 reads it, and without that read C-_ after a
	 * redo would reach dispatchMisc's plain doUndo() and undo the
	 * redo just applied -- so C-_ and C-/ would oscillate over one
	 * change instead of walking back up the redo stack.  Read from
	 * the dispatch path, not exercised against a running editor. */
	int micro;
	struct command *cmd;
	int cmd_count;
	struct editorRegister registers[127];
	struct buffer *lastVisitedBuffer;
	/* Universal argument.  Encodes three states in one int:
	 *   0            no argument pending
	 *   > 0          repeat count (C-u, C-u N, M-N)
	 *   UARG_REVERSE the M-- reverse modifier
	 * Commands that understand the reverse modifier (yank-pop,
	 * transpose, word case) test for UARG_REVERSE explicitly; all
	 * other commands must read the value through UARG_COUNT, which
	 * maps both 0 and UARG_REVERSE to a count of 1. */
	int uarg;

	struct history file_history;
	struct history command_history;
	struct history shell_history;
	struct history search_history;
	struct history replace_history;
	struct history rect_history;
	struct history kill_history;
	int kill_ring_pos;   /* Current position in kill ring for M-y */
	int self_insert_key; /* Stashed key for CMD_SELF_INSERT */
	struct timespec last_file_check; /* monotonic time of last
	                                  * checkFileModified syscall */
	struct abuf render_buf;		 /* Persistent screen-render buffer */
};

/*** prototypes ***/
void editorCleanup(void);
extern struct config E;
void handlePendingSignals(void);

/* Flag a resume so the next pass of the main loop reclaims the
 * terminal.  For code outside main.c that hands the tty back and
 * raises SIGTSTP itself: if the stop is discarded (orphaned process
 * group) the raise returns and the editor must repair its own
 * terminal state.  See openShellDrawer(). */
void requestTerminalResume(void);

#endif
