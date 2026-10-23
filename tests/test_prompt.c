/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_prompt.c: Prompt and minibuffer behaviour. */

#include "test.h"
#include "test_harness.h"
#include "buffer.h"
#include "prompt.h"
#include "keymap.h"
#include "unicode.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/* B1: a nested prompt must not clobber the outer prompt's saved
 * editor buffer.
 *
 * editorPrompt saves E.buf into E.edbuf on entry and restores from it
 * on exit.  E.edbuf was a single global slot with no save/restore, so
 * opening a second prompt from inside the first (C-x C-f, M-x, C-x b,
 * ...) overwrote the outer prompt's saved context with the minibuffer.
 * The outer prompt then restored E.buf = *minibuffer*, leaving every
 * later keystroke editing the minibuffer object while the windows
 * still showed the real file. */

void test_nested_prompt_preserves_buffer(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *file = make_test_buffer("real file contents");

	/* Inside the outer prompt: C-x C-f opens a nested Find File
	 * prompt, C-g cancels it, C-g cancels the outer one. */
	int keys[] = { KEY_CTRL('x'), KEY_CTRL('f'), KEY_CTRL('g'),
		       KEY_CTRL('g') };
	scriptKeys(keys, 4);

	muteStdout();
	uint8_t *r = editorPrompt("Find File: ", PROMPT_FILES, NULL);
	unmuteStdout();

	TEST_ASSERT_NULL(r);
	TEST_ASSERT(E.buf == file);
	TEST_ASSERT(E.buf != E.minibuf);

	free(r);
	clearKeys();
	freeMinibuffer();
	cleanupTestEditor();
}

/* The same thing one level deeper: three prompts on the stack. */
void test_double_nested_prompt_preserves_buffer(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *file = make_test_buffer("real file contents");

	int keys[] = { KEY_CTRL('x'), KEY_CTRL('f'), KEY_CTRL('x'),
		       KEY_CTRL('f'), KEY_CTRL('g'), KEY_CTRL('g'),
		       KEY_CTRL('g') };
	scriptKeys(keys, 7);

	muteStdout();
	uint8_t *r = editorPrompt("Find File: ", PROMPT_FILES, NULL);
	unmuteStdout();

	TEST_ASSERT(E.buf == file);
	TEST_ASSERT(E.buf != E.minibuf);

	free(r);
	clearKeys();
	freeMinibuffer();
	cleanupTestEditor();
}

/* B3: zap-to-char must not split a UTF-8 character.
 *
 * readKey() returns key tokens >= 1000 for navigation keys.  zapToChar
 * compared row bytes against (uint8_t)c, and truncating those tokens
 * lands in the UTF-8 lead-byte range -- KEY_ARROW_LEFT (1000) becomes
 * 0xE8, the lead byte of a 3-byte CJK sequence.  Deleting through
 * "that byte + 1" cut one byte into the character and left the buffer
 * holding invalid UTF-8, which save() then refuses entirely. */

void test_zap_arrow_key_does_not_corrupt_utf8(void) {
	initTestEditor();
	/* U+8BED (yu) is E8 AF AD -- lead byte 0xE8 == (uint8_t)1000. */
	struct buffer *buf = make_test_buffer("ab\xE8\xAF\xAD"
					      "cd");
	buf->cx = 0;
	buf->cy = 0;

	TEST_ASSERT_EQUAL_INT(1, utf8_validate(buf->row[0].chars,
					       buf->row[0].size));

	int keys[] = { KEY_ARROW_LEFT };
	scriptKeys(keys, 1);
	muteStdout();
	processKeypress(CMD_ZAP_TO_CHAR);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, utf8_validate(E.buf->row[0].chars,
					       E.buf->row[0].size));
	/* An arrow key is not a zap target, so nothing should be killed. */
	TEST_ASSERT_EQUAL_STRING("ab\xE8\xAF\xAD"
				 "cd",
				 row_str(E.buf, 0));

	cleanupTestEditor();
}

/* Meta keys truncate into the 2-byte lead range (2000 -> 0xD0). */
void test_zap_meta_key_does_not_corrupt_utf8(void) {
	initTestEditor();
	/* U+0416 is D0 96. */
	struct buffer *buf = make_test_buffer("ab\xD0\x96"
					      "cd");
	buf->cx = 0;
	buf->cy = 0;

	int keys[] = { KEY_META('P') }; /* 2000 + 'P' = 2080 -> 0x20 */
	keys[0] = KEY_META_BASE;	/* 2000 -> 0xD0 exactly */
	scriptKeys(keys, 1);
	muteStdout();
	processKeypress(CMD_ZAP_TO_CHAR);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, utf8_validate(E.buf->row[0].chars,
					       E.buf->row[0].size));
	TEST_ASSERT_EQUAL_STRING("ab\xD0\x96"
				 "cd",
				 row_str(E.buf, 0));

	cleanupTestEditor();
}

/* An ordinary ASCII zap must still work. */
void test_zap_ascii_still_works(void) {
	initTestEditor();
	struct buffer *buf = make_test_buffer("hello world");
	buf->cx = 0;
	buf->cy = 0;

	int keys[] = { 'o' };
	scriptKeys(keys, 1);
	muteStdout();
	processKeypress(CMD_ZAP_TO_CHAR);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_STRING(" world", row_str(E.buf, 0));

	cleanupTestEditor();
}


/* ---- TAB completion in the shell prompt (#131) ----
 *
 * A fixture directory holds bin/ (the only PATH entry) and work/ (the
 * current directory).  Each test types into the M-! prompt, presses
 * the given keys and RET, and checks the command that comes back. */

static char sh_root[256];
static char sh_cwd[4096];
static char *sh_saved_path;

/* An executable fixture is a real script, not an empty file: on a
 * filesystem mounted noacl, as MSYS2 mounts by default, Cygwin ignores
 * chmod's x bits and counts a file executable only if its name ends in
 * .exe, .com or .bat or its content starts with "#!". */
static void shTouch(const char *rel, mode_t mode) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", sh_root, rel);
	FILE *f = fopen(path, "w");
	if (f) {
		if (mode & 0111)
			fputs("#!/bin/sh\n", f);
		fclose(f);
	}
	chmod(path, mode);
}

static void shMkdir(const char *rel) {
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", sh_root, rel);
	mkdir(path, 0755);
}

static const char *const sh_files[] = {
	"bin/zqalpha", "bin/zqbeta", "bin/zqdata", "work/notes.txt",
	"work/My File.txt", "work/glob[1].txt", "work/report-2025.txt",
	"work/report-2026.txt", "work/.hidden", "work/src/main.c",
	"work/x\u0E01\u0E49\u0E32\u0E27.txt", /* xก้าว.txt */
	"work/x\u0E01\u0E34\u0E19.txt",	      /* xกิน.txt */
};

static int shFixtureUp(void) {
	snprintf(sh_root, sizeof(sh_root), "%s/emil_shcomp_XXXXXX",
		 testTmpDir());
	if (mkdtemp(sh_root) == NULL)
		return -1;
	shMkdir("bin");
	shMkdir("work");
	shMkdir("work/src");
	shTouch("bin/zqalpha", 0755);
	shTouch("bin/zqbeta", 0755);
	shTouch("bin/zqdata", 0644); /* not executable */
	for (size_t i = 3; i < sizeof(sh_files) / sizeof(sh_files[0]); i++)
		shTouch(sh_files[i], 0644);

	if (getcwd(sh_cwd, sizeof(sh_cwd)) == NULL)
		return -1;
	const char *path = getenv("PATH");
	sh_saved_path = path ? xstrdup(path) : NULL;
	char bin[512], work[512];
	snprintf(bin, sizeof(bin), "%s/bin", sh_root);
	snprintf(work, sizeof(work), "%s/work", sh_root);
	setenv("PATH", bin, 1);
	return chdir(work);
}

static void shFixtureDown(void) {
	if (chdir(sh_cwd) != 0)
		perror("chdir");
	if (sh_saved_path) {
		setenv("PATH", sh_saved_path, 1);
		free(sh_saved_path);
		sh_saved_path = NULL;
	}
	char path[512];
	for (size_t i = 0; i < sizeof(sh_files) / sizeof(sh_files[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", sh_root, sh_files[i]);
		unlink(path);
	}
	const char *dirs[] = { "work/src", "work", "bin", "" };
	for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", sh_root, dirs[i]);
		rmdir(path);
	}
}

/* Type 'typed', then the keys in 'after' (up to a 0), then RET.
 * Returns what a prompt of type t hands back; caller frees. */
static char *promptAfter(enum promptType t, const char *typed,
			 const int *after) {
	int keys[64];
	int n = 0;
	for (const char *p = typed; *p && n < 60; p++)
		keys[n++] = (unsigned char)*p;
	for (; after && *after && n < 63; after++)
		keys[n++] = *after;
	keys[n++] = '\r';
	scriptKeys(keys, n);

	muteStdout();
	uint8_t *r = editorPrompt("Prompt: ", t, NULL);
	unmuteStdout();
	clearKeys();
	return (char *)r;
}

static const int TAB_ONCE[] = { '\t', 0 };

#define PROMPT_CASE(t, typed, keys, expected)                               \
	do {                                                               \
		char *got_ = promptAfter((t), (typed), (keys));            \
		TEST_ASSERT_NOT_NULL(got_);                                \
		if (got_)                                                  \
			TEST_ASSERT_EQUAL_STRING((expected), got_);        \
		free(got_);                                                \
	} while (0)
#define SHELL_CASE(typed, keys, expected)                                   \
	PROMPT_CASE(PROMPT_SHELL, typed, keys, expected)

static void shellCompletionSetUp(void) {
	initTestEditor();
	makeMinibuffer();
	make_test_buffer("");
}

static void shellCompletionTearDown(void) {
	freeMinibuffer();
	cleanupTestEditor();
}

/* Command position: PATH is searched, and only executables count. */
void test_shell_tab_completes_command(void) {
	TEST_ASSERT_EQUAL_INT(0, shFixtureUp());
	shellCompletionSetUp();

	SHELL_CASE("zqa", TAB_ONCE, "zqalpha ");
	/* A platform with no exec bit (WASI, Genode: chmod is not
	 * implemented) reports every file executable, so the 0644 file
	 * still completes there.  Probe for that instead of listing
	 * platforms. */
	char probe[512];
	snprintf(probe, sizeof(probe), "%s/bin/zqdata", sh_root);
	if (access(probe, X_OK) != 0)
		SHELL_CASE("zqd", TAB_ONCE, "zqd"); /* not executable */
	SHELL_CASE("notes", TAB_ONCE, "notes"); /* files are not commands */
	SHELL_CASE("  zqb", TAB_ONCE, "  zqbeta ");

	shellCompletionTearDown();
	shFixtureDown();
}

/* Every way a new command starts puts the next word in command
 * position; a variable assignment or a reserved word keeps it there. */
void test_shell_tab_command_position(void) {
	TEST_ASSERT_EQUAL_INT(0, shFixtureUp());
	shellCompletionSetUp();

	SHELL_CASE("cat notes.txt | zqa", TAB_ONCE, "cat notes.txt | zqalpha ");
	SHELL_CASE("true && zqa", TAB_ONCE, "true && zqalpha ");
	SHELL_CASE("true;zqa", TAB_ONCE, "true;zqalpha ");
	SHELL_CASE("x=$(zqa", TAB_ONCE, "x=$(zqalpha ");
	SHELL_CASE("FOO=1 BAR=2 zqa", TAB_ONCE, "FOO=1 BAR=2 zqalpha ");
	SHELL_CASE("if zqa", TAB_ONCE, "if zqalpha ");
	SHELL_CASE("zqalpha zqa", TAB_ONCE, "zqalpha zqa"); /* an argument */
	SHELL_CASE("zqalpha 2>&1 | zqb", TAB_ONCE, "zqalpha 2>&1 | zqbeta ");
	SHELL_CASE("FOO=\"a b\" zqa", TAB_ONCE, "FOO=\"a b\" zqalpha ");
	SHELL_CASE(">out zqa", TAB_ONCE, ">out zqalpha "); /* after a target */
	SHELL_CASE("zqalpha >&no", TAB_ONCE, "zqalpha >&notes.txt ");
	SHELL_CASE("\"if\" zqa", TAB_ONCE, "\"if\" zqa"); /* quoted: a command */

	shellCompletionTearDown();
	shFixtureDown();
}

/* Anywhere else, file names: quoted for where they land. */
void test_shell_tab_completes_files(void) {
	TEST_ASSERT_EQUAL_INT(0, shFixtureUp());
	shellCompletionSetUp();

	SHELL_CASE("cat no", TAB_ONCE, "cat notes.txt ");
	SHELL_CASE("cat sr", TAB_ONCE, "cat src/"); /* no space: a dir */
	SHELL_CASE("cat src/", TAB_ONCE, "cat src/main.c ");
	SHELL_CASE("cat My", TAB_ONCE, "cat My\\ File.txt ");
	SHELL_CASE("cat \"My", TAB_ONCE, "cat \"My File.txt\" ");
	SHELL_CASE("cat 'My", TAB_ONCE, "cat 'My File.txt' ");
	SHELL_CASE("cat My\\ F", TAB_ONCE, "cat My\\ File.txt ");
	SHELL_CASE("cat gl", TAB_ONCE, "cat glob\\[1\\].txt ");
	SHELL_CASE("cat rep", TAB_ONCE, "cat report-202"); /* common part */
	SHELL_CASE("cat .h", TAB_ONCE, "cat .hidden ");
	SHELL_CASE("zqalpha > no", TAB_ONCE, "zqalpha > notes.txt ");
	SHELL_CASE("./sr", TAB_ONCE, "./src/"); /* a path, not a command */
	SHELL_CASE("zqalpha --file=no", TAB_ONCE, "zqalpha --file=notes.txt ");
	SHELL_CASE("cat nothing", TAB_ONCE, "cat nothing");

	shellCompletionTearDown();
	shFixtureDown();
}

/* An empty PATH entry is the current directory. */
void test_shell_tab_empty_path_entry(void) {
	TEST_ASSERT_EQUAL_INT(0, shFixtureUp());
	shellCompletionSetUp();

	char path[600];
	snprintf(path, sizeof(path), "%s/bin:", sh_root);
	setenv("PATH", path, 1);
	shTouch("work/zqlocal", 0755);
	SHELL_CASE("zql", TAB_ONCE, "zqlocal ");
	snprintf(path, sizeof(path), "%s/work/zqlocal", sh_root);
	unlink(path);

	shellCompletionTearDown();
	shFixtureDown();
}

/* The file prompts share the shell prompt's file lister.  A name is
 * literal text, not a glob pattern, and a common prefix never ends
 * inside a UTF-8 character. */
void test_file_prompt_completion(void) {
	TEST_ASSERT_EQUAL_INT(0, shFixtureUp());
	shellCompletionSetUp();

	PROMPT_CASE(PROMPT_FILES, "no", TAB_ONCE, "notes.txt");
	PROMPT_CASE(PROMPT_FILES, "glob[1]", TAB_ONCE, "glob[1].txt");
	PROMPT_CASE(PROMPT_FILES, "src/m", TAB_ONCE, "src/main.c");
	PROMPT_CASE(PROMPT_FILES, "x", TAB_ONCE, "x\u0E01");
	PROMPT_CASE(PROMPT_DIR, "s", TAB_ONCE, "src"); /* RET drops the /  */
	PROMPT_CASE(PROMPT_DIR, "no", TAB_ONCE, "no"); /* not a directory */
	SHELL_CASE("cat x", TAB_ONCE, "cat x\u0E01");

	shellCompletionTearDown();
	shFixtureDown();
}

/* Point in mid-line: the word before point is completed, the rest of
 * the line is left alone, and an existing space is stepped over. */
void test_shell_tab_mid_line(void) {
	TEST_ASSERT_EQUAL_INT(0, shFixtureUp());
	shellCompletionSetUp();

	const int keys[] = { KEY_CTRL('b'), KEY_CTRL('b'), KEY_CTRL('b'),
			     KEY_CTRL('b'), KEY_CTRL('b'), '\t', 'X', 0 };
	SHELL_CASE("cat no | wc", keys, "cat notes.txt X| wc");

	shellCompletionTearDown();
	shFixtureDown();
}

/* A literal TAB is still available, and a completion undoes as one. */
void test_shell_tab_quoted_insert_and_undo(void) {
	TEST_ASSERT_EQUAL_INT(0, shFixtureUp());
	shellCompletionSetUp();

	const int quoted[] = { KEY_CTRL('q'), '\t', 0 };
	SHELL_CASE("a", quoted, "a\t");

	const int undo[] = { '\t', KEY_CTRL('_'), 0 };
	SHELL_CASE("cat no", undo, "cat no");

	shellCompletionTearDown();
	shFixtureDown();
}

void test_history_up_drops_stale_mark(void) {
	initTestEditor();
	makeMinibuffer();
	make_test_buffer("");
	addHistory(&E.search_history, ")\xE3\x80\x82");

	int keys[] = { 'o', '6', KEY_CTRL('@'), KEY_ARROW_UP, KEY_CTRL('w'),
		       '\r' };
	scriptKeys(keys, 6);
	muteStdout();
	char *got = (char *)editorPrompt("Search: ", PROMPT_SEARCH, NULL);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_NOT_NULL(got);
	if (got)
		TEST_ASSERT_EQUAL_INT(1, utf8_validate((uint8_t *)got,
						       (int)strlen(got)));
	free(got);
	freeHistory(&E.search_history);
	freeMinibuffer();
	cleanupTestEditor();
}

/* These tests manage the editor themselves. */
void setUp(void) {
}

void tearDown(void) {
}

/* confirmYN's answer is recorded for keyboard macros, so a macro that
 * answers a y-or-n question replays the same answer. */
void test_confirm_yn_answer_recorded_in_macro(void) {
	initTestEditor();
	make_test_buffer("text"); /* refreshScreen() draws the focused buffer */
	E.macro.skeys = 32;
	E.macro.nkeys = 0;
	E.macro.keys = xmalloc(E.macro.skeys * sizeof(int));
	E.recording = 1;
	const int keys[] = { 'y', 'n' };
	scriptKeys(keys, 2);

	muteStdout();
	int first = confirmYN("Really? (y or n)");
	int second = confirmYN("Really? (y or n)");
	unmuteStdout();

	TEST_ASSERT_EQUAL_INT(1, first);
	TEST_ASSERT_EQUAL_INT(0, second);
	TEST_ASSERT_EQUAL_INT(2, E.macro.nkeys);
	TEST_ASSERT_EQUAL_INT('y', E.macro.keys[0]);
	TEST_ASSERT_EQUAL_INT('n', E.macro.keys[1]);

	E.recording = 0;
	free(E.macro.keys);
	E.macro.keys = NULL;
	E.macro.nkeys = 0;
	clearKeys();
	cleanupTestEditor();
}

/* ---- Buffer names the minibuffer cannot hold as they are ---- */

/* '\n' is emil's row separator, never a byte in a row, but a filename
 * can hold one, and an escape or a byte that is not UTF-8 besides.
 * The buffer prompt completes and lists filenames, so each has a form
 * the minibuffer and the list can show. */
static const struct {
	const char *name, *shown;
} bad_names[] = {
	{ "qq\nyy", "qq^Jyy" },
	{ "qq\x1b[0my", "qq^[[0my" },
	{ "qq\xffy", "qq\xef\xbf\xbdy" },
};

/* A second buffer under `cur`, called `name`.  Linked in by hand:
 * these names cannot be typed at a file prompt. */
static struct buffer *addBufferNamed(struct buffer *after, const char *name) {
	struct buffer *b = newBuffer();
	b->filename = xstrdup(name);
	b->next = after->next;
	after->next = b;
	return b;
}

/* Answer a buffer prompt with `keys`; returns what it hands back. */
static char *bufferPrompt(const int *keys, int n) {
	scriptKeys(keys, n);
	muteStdout();
	uint8_t *r = editorPrompt("Buffer: ", PROMPT_BUFFER, NULL);
	unmuteStdout();
	TEST_ASSERT_EQUAL_INT(n, test_key_pos);
	clearKeys();
	return (char *)r;
}

static int frameHas(const char *want) {
	int n = (int)strlen(want);
	for (int i = 0; i + n <= E.render_buf.len; i++)
		if (memcmp(E.render_buf.b + i, want, n) == 0)
			return 1;
	return 0;
}

/* TAB on a name with one match puts the name in the minibuffer in
 * its showable form, and leaves the buffer's own name alone. */
void test_completion_inserts_stored_name_as_text(void) {
	for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
		initTestEditor();
		makeMinibuffer();
		struct buffer *cur = make_test_buffer("x");
		struct buffer *b = addBufferNamed(cur, bad_names[i].name);

		/* q q TAB RET: what RET returns is what TAB left. */
		const int keys[] = { 'q', 'q', '\t', '\r' };
		char *got = bufferPrompt(keys, 4);

		TEST_ASSERT_NOT_NULL(got);
		if (got) {
			TEST_ASSERT_EQUAL_STRING(bad_names[i].shown, got);
			TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)got,
						       (int)strlen(got)));
		}
		TEST_ASSERT_EQUAL_STRING(bad_names[i].name, b->filename);
		free(got);
		freeMinibuffer();
		cleanupTestEditor();
	}
}

/* Choosing from the list does the same.  Two names share the prefix,
 * so a second TAB lists them; Down twice goes to "qqz" and round to
 * the first again. */
void test_completion_cycle_inserts_stored_name_as_text(void) {
	for (size_t i = 0; i < sizeof(bad_names) / sizeof(bad_names[0]); i++) {
		initTestEditor();
		makeMinibuffer();
		struct buffer *cur = make_test_buffer("x");
		struct buffer *b = addBufferNamed(cur, bad_names[i].name);
		addBufferNamed(b, "qqz");

		/* Typing a character after choosing returns the line as
		 * the minibuffer holds it: the showable form, plus X. */
		const int keys[] = { 'q', 'q', '\t', '\t', KEY_ARROW_DOWN,
				     KEY_ARROW_DOWN, 'X', '\r' };
		char *got = bufferPrompt(keys, 8);
		char want[32];
		snprintf(want, sizeof(want), "%sX", bad_names[i].shown);
		TEST_ASSERT_NOT_NULL(got);
		if (got) {
			TEST_ASSERT_EQUAL_STRING(want, got);
			TEST_ASSERT_TRUE(utf8_validate((const uint8_t *)got,
						       (int)strlen(got)));
		}
		free(got);

		/* And the choice still names the real buffer: C-x b,
		 * the same keys without the X, switches to it. */
		const int pick[] = { 'q', 'q', '\t', '\t', KEY_ARROW_DOWN,
				     KEY_ARROW_DOWN, '\r' };
		scriptKeys(pick, 7);
		muteStdout();
		processKeypress(CMD_SWITCH_BUFFER);
		unmuteStdout();
		clearKeys();
		TEST_ASSERT(E.buf == b);
		TEST_ASSERT_EQUAL_STRING(bad_names[i].name, b->filename);

		freeMinibuffer();
		cleanupTestEditor();
	}
}

/* What was typed before a file match isn't a stored name: a tab in a
 * directory stays a tab, or the path would no longer exist. */
void test_file_completion_keeps_typed_prefix(void) {
	char dir[PATH_MAX];
	testTmpPath(dir, sizeof(dir), "emil-prompt-XXXXXX");
	TEST_ASSERT_NOT_NULL(mkdtemp(dir));
	char sub[PATH_MAX], file[PATH_MAX], typed[PATH_MAX];
	int r = snprintf(sub, sizeof(sub), "%s/a\tb", dir);
	TEST_ASSERT(r >= 0 && (size_t)r < sizeof(sub));
	TEST_ASSERT_EQUAL_INT(0, mkdir(sub, 0700));
	r = snprintf(file, sizeof(file), "%s/file", sub);
	TEST_ASSERT(r >= 0 && (size_t)r < sizeof(file));
	FILE *f = fopen(file, "w");
	TEST_ASSERT_NOT_NULL(f);
	fclose(f);

	initTestEditor();
	makeMinibuffer();
	(void)make_test_buffer("x");
	r = snprintf(typed, sizeof(typed), "%s/fi", sub);
	TEST_ASSERT(r >= 0 && (size_t)r < sizeof(typed));

	/* Type the path up to "fi", the tab in it as C-q TAB; then TAB
	 * to complete, and RET. */
	static int keys[TEST_KEY_SCRIPT_MAX];
	int n = 0;
	for (const char *c = typed; *c; c++) {
		if (*c == '\t')
			keys[n++] = KEY_CTRL('q');
		keys[n++] = (unsigned char)*c;
	}
	keys[n++] = '\t';
	keys[n++] = '\r';
	scriptKeys(keys, n);
	muteStdout();
	char *got = (char *)editorPrompt("File: ", PROMPT_FILES, NULL);
	unmuteStdout();
	TEST_ASSERT_EQUAL_INT(n, test_key_pos);
	clearKeys();

	TEST_ASSERT_NOT_NULL(got);
	if (got)
		TEST_ASSERT_EQUAL_STRING(file, got);
	free(got);

	freeMinibuffer();
	cleanupTestEditor();
	unlink(file);
	rmdir(sub);
	rmdir(dir);
}

/* A second TAB lists both matches.  The list is drawn while the
 * prompt waits, so the frame on screen when RET arrives shows it:
 * the awkward name in its one-line form, beside the plain one. */
void test_buffer_prompt_lists_newline_name_on_one_row(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *cur = make_test_buffer("x");
	struct buffer *b = addBufferNamed(cur, "qq\nyy");
	addBufferNamed(b, "qqz");

	const int keys[] = { 'q', 'q', '\t', '\t', '\r' };
	char *got = bufferPrompt(keys, 5);
	free(got);

	TEST_ASSERT(E.render_buf.len > 0);
	TEST_ASSERT(frameHas("qq^Jyy"));
	TEST_ASSERT(frameHas("qqz"));
	TEST_ASSERT(memchr(E.render_buf.b, '\0', E.render_buf.len) == NULL);

	freeMinibuffer();
	cleanupTestEditor();
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_completion_inserts_stored_name_as_text);
	RUN_TEST(test_completion_cycle_inserts_stored_name_as_text);
	RUN_TEST(test_file_completion_keeps_typed_prefix);
	RUN_TEST(test_buffer_prompt_lists_newline_name_on_one_row);
	RUN_TEST(test_nested_prompt_preserves_buffer);
	RUN_TEST(test_double_nested_prompt_preserves_buffer);
	RUN_TEST(test_zap_arrow_key_does_not_corrupt_utf8);
	RUN_TEST(test_zap_meta_key_does_not_corrupt_utf8);
	RUN_TEST(test_zap_ascii_still_works);
	RUN_TEST(test_shell_tab_completes_command);
	RUN_TEST(test_shell_tab_command_position);
	RUN_TEST(test_shell_tab_completes_files);
	RUN_TEST(test_shell_tab_empty_path_entry);
	RUN_TEST(test_file_prompt_completion);
	RUN_TEST(test_shell_tab_mid_line);
	RUN_TEST(test_shell_tab_quoted_insert_and_undo);
	RUN_TEST(test_history_up_drops_stale_mark);
	RUN_TEST(test_confirm_yn_answer_recorded_in_macro);

	return TEST_END();
}
