/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_prompt.c: Prompt and minibuffer behaviour. */

#include "test.h"
#include "test_harness.h"
#include "prompt.h"
#include "keymap.h"
#include "unicode.h"
#include "edit.h"
#include "completion.h"
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
	int keys[] = { CTRL('x'), CTRL('f'), CTRL('g'), CTRL('g') };
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

	int keys[] = { CTRL('x'), CTRL('f'), CTRL('x'), CTRL('f'),
		       CTRL('g'), CTRL('g'), CTRL('g') };
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
	zapToChar();
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
	zapToChar();
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
	zapToChar();
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
};

static int shFixtureUp(void) {
	const char *tmp = getenv("TMPDIR");
	snprintf(sh_root, sizeof(sh_root), "%s/emil_shcomp_XXXXXX",
		 tmp && *tmp ? tmp : "/tmp");
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
 * Returns what the shell prompt hands back; caller frees. */
static char *shellPromptAfter(const char *typed, const int *after) {
	int keys[64];
	int n = 0;
	for (const char *p = typed; *p && n < 60; p++)
		keys[n++] = (unsigned char)*p;
	for (; after && *after && n < 63; after++)
		keys[n++] = *after;
	keys[n++] = '\r';
	scriptKeys(keys, n);

	muteStdout();
	uint8_t *r = editorPrompt("Shell: ", PROMPT_SHELL, NULL);
	unmuteStdout();
	clearKeys();
	return (char *)r;
}

static const int TAB_ONCE[] = { '\t', 0 };

#define SHELL_CASE(typed, keys, expected)                                   \
	do {                                                               \
		char *got_ = shellPromptAfter((typed), (keys));            \
		TEST_ASSERT_NOT_NULL(got_);                                \
		if (got_)                                                  \
			TEST_ASSERT_EQUAL_STRING((expected), got_);        \
		free(got_);                                                \
	} while (0)

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

/* Point in mid-line: the word before point is completed, the rest of
 * the line is left alone, and an existing space is stepped over. */
void test_shell_tab_mid_line(void) {
	TEST_ASSERT_EQUAL_INT(0, shFixtureUp());
	shellCompletionSetUp();

	const int keys[] = { CTRL('b'), CTRL('b'), CTRL('b'), CTRL('b'),
			     CTRL('b'), '\t', 'X', 0 };
	SHELL_CASE("cat no | wc", keys, "cat notes.txt X| wc");

	shellCompletionTearDown();
	shFixtureDown();
}

/* A literal TAB is still available, and a completion undoes as one. */
void test_shell_tab_quoted_insert_and_undo(void) {
	TEST_ASSERT_EQUAL_INT(0, shFixtureUp());
	shellCompletionSetUp();

	const int quoted[] = { CTRL('q'), '\t', 0 };
	SHELL_CASE("a", quoted, "a\t");

	const int undo[] = { '\t', CTRL('_'), 0 };
	SHELL_CASE("cat no", undo, "cat no");

	shellCompletionTearDown();
	shFixtureDown();
}

/* These tests manage the editor themselves. */
void setUp(void) {
}

void tearDown(void) {
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_nested_prompt_preserves_buffer);
	RUN_TEST(test_double_nested_prompt_preserves_buffer);
	RUN_TEST(test_zap_arrow_key_does_not_corrupt_utf8);
	RUN_TEST(test_zap_meta_key_does_not_corrupt_utf8);
	RUN_TEST(test_zap_ascii_still_works);
	RUN_TEST(test_shell_tab_completes_command);
	RUN_TEST(test_shell_tab_command_position);
	RUN_TEST(test_shell_tab_completes_files);
	RUN_TEST(test_shell_tab_mid_line);
	RUN_TEST(test_shell_tab_quoted_insert_and_undo);

	return TEST_END();
}
