#ifndef EMIL_KEYMAP_H
#define EMIL_KEYMAP_H

/* Key constants */
#ifndef CTRL
#define CTRL(x) ((x) & 0x1f)
#endif

/*
 * Key tokens — physical key identities returned by the terminal layer.
 * These represent what key was pressed, not what it means.
 */
enum keyToken {
	KEY_BACKSPACE = 127,
	KEY_ARROW_LEFT = 1000,
	KEY_ARROW_RIGHT,
	KEY_ARROW_UP,
	KEY_ARROW_DOWN,
	KEY_HOME,
	KEY_DEL,
	KEY_END,
	KEY_PAGE_UP,
	KEY_PAGE_DOWN,
	KEY_BACKTAB,
	KEY_UNICODE,
	KEY_UNICODE_ERROR,
	/* Meta + character: KEY_META_BASE + character value.
	 * e.g. Meta-f = KEY_META_BASE + 'f' */
	KEY_META_BASE = 2000,
	/* Alt digits for universal argument */
	KEY_ALT_0 = 3000,
	KEY_ALT_1,
	KEY_ALT_2,
	KEY_ALT_3,
	KEY_ALT_4,
	KEY_ALT_5,
	KEY_ALT_6,
	KEY_ALT_7,
	KEY_ALT_8,
	KEY_ALT_9,
};

/* Helper to construct and test Meta key tokens */
#define KEY_META(ch) (KEY_META_BASE + (ch))
#define IS_META_KEY(k) ((k) >= KEY_META_BASE && (k) < KEY_ALT_0)
#define META_CHAR(k) ((k) - KEY_META_BASE)

/*
 * Command tokens — editor actions produced by the binding layer.
 * These represent what the editor should do.
 */
enum command_t {
	/* Returned when binding is incomplete (need more keys) */
	CMD_NONE = 0,
	CMD_FORWARD_CHAR = 4000,
	CMD_BACKWARD_CHAR,
	CMD_NEXT_LINE,
	CMD_PREV_LINE,
	CMD_PAGE_UP,
	CMD_PAGE_DOWN,
	CMD_SCROLL_UP,
	CMD_SCROLL_DOWN,
	CMD_HOME,
	CMD_END,
	CMD_BEG_OF_FILE,
	CMD_END_OF_FILE,
	CMD_FORWARD_WORD,
	CMD_BACKWARD_WORD,
	CMD_FORWARD_PARA,
	CMD_BACKWARD_PARA,
	CMD_FORWARD_SEXP,
	CMD_BACKWARD_SEXP,
	CMD_SENTENCE_FORWARD,
	CMD_SENTENCE_BACKWARD,
	CMD_RECENTER,
	CMD_GOTO_LINE,
	/* Editing */
	CMD_NEWLINE,
	CMD_BACKSPACE,
	CMD_HELP,
	CMD_DELETE,
	CMD_UNICODE,
	CMD_UNICODE_ERROR,
	CMD_KILL_LINE,
	CMD_NEWLINE_INDENT,
	CMD_OPEN_LINE,
	CMD_QUOTED_INSERT,
	CMD_DELETE_WORD,
	CMD_BACKSPACE_WORD,
	CMD_UPCASE_WORD,
	CMD_DOWNCASE_WORD,
	CMD_CAPCASE_WORD,
	CMD_TRANSPOSE_CHARS,
	CMD_TRANSPOSE_WORDS,
	CMD_TRANSPOSE_SENTENCES,
	CMD_ZAP_TO_CHAR,
	CMD_KILL_SEXP,
	CMD_KILL_PARA,
	CMD_MARK_PARA,
	CMD_UNINDENT,
	CMD_SELF_INSERT,
	CMD_TAB,
	/* Window management */
	CMD_OTHER_WINDOW,
	CMD_CREATE_WINDOW,
	CMD_DESTROY_WINDOW,
	CMD_DESTROY_OTHER_WINDOWS,
	/* Buffers and files */
	CMD_QUIT,
	CMD_SAVE,
	CMD_SAVE_AS,
	CMD_SWITCH_BUFFER,
	CMD_NEXT_BUFFER,
	CMD_PREV_BUFFER,
	CMD_MARK_BUFFER,
	CMD_KILL_BUFFER,
	CMD_INSERT_FILE,
	CMD_FIND_FILE,
	CMD_FIND_FILE_READ_ONLY,
	CMD_TOGGLE_READ_ONLY,
	CMD_REDO,
	/* Region, registers, rectangles */
	CMD_SET_MARK,
	CMD_SWAP_MARK,
	CMD_CUT,
	CMD_COPY,
	CMD_COPY_CLIPBOARD,
	CMD_YANK,
	CMD_YANK_POP,
	CMD_KILL_REGION,
	CMD_UPCASE_REGION,
	CMD_DOWNCASE_REGION,
	CMD_JUMP_REGISTER,
	CMD_POINT_REGISTER,
	CMD_REGION_REGISTER,
	CMD_INC_REGISTER,
	CMD_INSERT_REGISTER,
	CMD_VIEW_REGISTER,
	CMD_STRING_RECT,
	CMD_COPY_RECT,
	CMD_KILL_RECT,
	CMD_YANK_RECT,
	CMD_RECT_REGISTER,
	CMD_TOGGLE_RECT_MODE,
	/* Search */
	CMD_ISEARCH,
	CMD_REVERSE_ISEARCH,
	CMD_REGEX_SEARCH_FORWARD,
	CMD_REGEX_SEARCH_BACKWARD,
	CMD_QUERY_REPLACE,
	/* Macros */
	CMD_MACRO_RECORD,
	CMD_MACRO_END,
	CMD_MACRO_EXEC,
	/* Misc */
	CMD_UNDO,
	CMD_SUSPEND,
	CMD_SHELL_DRAWER,
	CMD_CTAGS_JUMP,
	CMD_CTAGS_BACK,
	CMD_TOGGLE_HEADER_BODY,
	CMD_VISUAL_LINE_MODE,
	CMD_WHAT_CURSOR,
	CMD_EXEC_CMD,
	CMD_CUSTOM_INFO,
	CMD_CANCEL,
	CMD_UNIVERSAL_ARG,
	CMD_PIPE_CMD,
	CMD_SHELL_CMD,
	CMD_KILL_LINE_BACKWARDS,
	CMD_EXPAND,
	/* Internal: unknown command — argument is the key */
	CMD_UNKNOWN,
};

/* Prefix state for key sequences */
enum PrefixState { PREFIX_NONE, PREFIX_CTRL_X, PREFIX_CTRL_X_R };

struct buffer;
struct macro;
struct config;

/* Function declarations */
void recordKey(int c);
void processKeypress(int cmd);
void execMacro(struct macro *macro);
void setupCommands(void);
void runCommand(char *cmd);
int resolveBinding(int key);
void showPrefix(const char *prefix);

#endif /* EMIL_KEYMAP_H */
