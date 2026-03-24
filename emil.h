#ifndef EMIL_H
#define EMIL_H 1

#include "keymap.h"
#include <stdint.h>
#include <stdlib.h>
#include <termios.h>
#include <time.h>

/*** util ***/

#define EMIL_TAB_STOP 8

#ifndef EMIL_VERSION
#define EMIL_VERSION "unknown"
#endif

#define ESC "\033"
#define CSI ESC "["
#define CRLF "\r\n"

/* Suppress GCC's warn_unused_result where the return value is
 * intentionally discarded (e.g. best-effort write to stdout). */
#define IGNORE_RETURN(expr) \
	do {                \
		if (expr) { \
		}           \
	} while (0)
#define ISCTRL(c) ((0 < c && c < 0x20) || c == 0x7f)

enum promptType {
	PROMPT_BASIC,
	PROMPT_BUFFER,
	PROMPT_FILES,
	PROMPT_DIR,
	PROMPT_COMMAND,
	PROMPT_SEARCH,
};
/*** data ***/

typedef struct erow {
	int size;
	uint8_t *chars;
	int cached_width; /* display width in columns, or -1 if stale */
} erow;

struct undo {
	struct undo *prev;
	int startx;
	int starty;
	int endx;
	int endy;
	int append;
	int datalen;
	int datasize;
	int delete;
	int paired;
	uint8_t *data;
};

struct completion_state {
	char *last_completed_text;
	int completion_start_pos;
	int successive_tabs;
	int last_completion_count;
	int preserve_message;
	int selected;	/* Currently highlighted match index, -1 = none */
	char **matches; /* Copy of match list for M-n/M-p navigation */
	int n_matches;	/* Number of matches in the list */
};

struct completion_result {
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
	int indent;
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
	int single_line;
	int read_only;
	int lock_fd;	   /* fd holding advisory lock, or -1 */
	time_t open_mtime; /* st_mtime at open/save, 0 if unset */
	int external_mod;  /* 1 if file changed on disk since open/save */
	int internal_mod;
	erow *row;
	char *filename;
	char *display_name; /* Truncated name for status bar display */
	int min_name_len;   /* Min chars to show without colliding */
	uint8_t *query;
	uint8_t match;
	struct undo *undo;
	struct undo *redo;
	int undo_count;
	int undo_pruned;
	struct buffer *next;
	int *screen_line_start;
	int screen_line_cache_size;
	int screen_line_cache_valid;
	struct completion_state completion_state;
};

struct window {
	int focused;
	struct buffer *buf;
	int scx, scy;
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
	struct text kill; /* active kill entry */
	int screenrows;
	int screencols;
	uint8_t unicode[4];
	int nunicode;
	char statusmsg[1024];
	char prefix_display[32]; /* Display prefix commands like C-u */

	/* Buffer management for minibuffer */
	struct buffer *edbuf;	/* Saved editor context */
	struct buffer *minibuf; /* Minibuffer object */

	time_t statusmsg_time;
	struct termios orig_termios;
	struct buffer *headbuf;
	struct buffer *buf; /* Current active buffer */
	int nwindows;
	struct window **windows;
	int recording;
	struct macro macro;
	int playback;
	int micro;
	struct command *cmd;
	int cmd_count;
	struct editorRegister registers[127];
	struct buffer *lastVisitedBuffer;
	int uarg; /* Universal argument: 0 = off, non-zero = active with that value */
	int macro_depth; /* Current macro execution depth to prevent infinite
                      recursion */

	struct history file_history;
	struct history command_history;
	struct history shell_history;
	struct history search_history;
	struct history kill_history;
	int kill_ring_pos;   /* Current position in kill ring for M-y */
	int self_insert_key; /* Stashed key for CMD_SELF_INSERT */
};

/*** prototypes ***/

uint8_t *editorPrompt(struct buffer *bufr, const uint8_t *prompt,
		      enum promptType t,
		      void (*callback)(struct buffer *, uint8_t *, int));
void updateBuffer(struct buffer *buf);
void insertNewlineRaw(void);
void insertNewline(int count);
void insertChar(struct buffer *bufr, int c, int count);
int editorOpen(struct buffer *bufr, char *filename);
void die(const char *s);
struct buffer *newBuffer(void);
void destroyBuffer(struct buffer *);
void recordKey(int c);
void execMacro(struct macro *macro);

#endif
