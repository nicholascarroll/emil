/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_regress.c: Regression tests for previously-fixed defects.
 *
 * Each test here pins behaviour that was once wrong.  They are grouped
 * by the defect they guard rather than by module, because the point of
 * the file is "this must not come back" rather than "this module
 * works".  Reference the original symptom in the comment so a future
 * reader can tell an intentional behaviour change from a regression. */

#include "test.h"
#include "test_harness.h"
#include "prompt.h"
#include "fileio.h"
#include "find.h"
#include "edit.h"
#include "keymap.h"
#include "unicode.h"
#include "util.h"
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

/* Keys fed to readKey() by the stub in stubs.c. */
extern int test_key_script[64];
extern int test_key_count;
extern int test_key_pos;

static void scriptKeys(const int *keys, int n) {
	for (int i = 0; i < n; i++)
		test_key_script[i] = keys[i];
	test_key_count = n;
	test_key_pos = 0;
}

static void clearKeys(void) {
	test_key_count = 0;
	test_key_pos = 0;
}

/* The prompt loop and the search callback both call refreshScreen(),
 * which writes escape sequences straight to fd 1.  That would corrupt
 * the test runner's view of stdout, so redirect it for the duration of
 * the call under test. */
static int saved_stdout = -1;

static void muteStdout(void) {
	fflush(stdout);
	saved_stdout = dup(STDOUT_FILENO);
	int devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, STDOUT_FILENO);
		close(devnull);
	}
}

static void unmuteStdout(void) {
	fflush(stdout);
	if (saved_stdout >= 0) {
		dup2(saved_stdout, STDOUT_FILENO);
		close(saved_stdout);
		saved_stdout = -1;
	}
}

/* editorPrompt needs a minibuffer; main.c builds one at startup and
 * initTestEditor does not. */
static void makeMinibuffer(void) {
	E.minibuf = newBuffer();
	E.minibuf->word_wrap = 0;
	E.minibuf->filename = xstrdup("*minibuffer*");
	E.minibuf->special_buffer = 1;
}

static void freeMinibuffer(void) {
	if (E.minibuf) {
		destroyBuffer(E.minibuf);
		E.minibuf = NULL;
	}
}

/* Write a scratch file and return a malloc'd path. */
static char *writeTempFile(const char *name, const char *contents) {
	char *path = xmalloc(256);
	snprintf(path, 256, "/tmp/emil_regress_%s_%d", name, (int)getpid());
	FILE *fp = fopen(path, "w");
	if (fp) {
		fputs(contents, fp);
		fclose(fp);
	}
	return path;
}

/* ---- B1: a nested prompt must not clobber the outer prompt's saved
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
	uint8_t *r = editorPrompt(file, "Find File: ", PROMPT_FILES, NULL);
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
	uint8_t *r = editorPrompt(file, "Find File: ", PROMPT_FILES, NULL);
	unmuteStdout();

	TEST_ASSERT(E.buf == file);
	TEST_ASSERT(E.buf != E.minibuf);

	free(r);
	clearKeys();
	freeMinibuffer();
	cleanupTestEditor();
}

/* ---- B2: revert() on a buffer with no filename must not crash.
 *
 * revert() passed buf->filename straight to editorOpen, whose first
 * act is collapseHome(filename) -> path[0].  Starting emil with no
 * arguments leaves the initial buffer with filename == NULL, so
 * M-x revert-buffer segfaulted. */

void test_revert_null_filename_survives(void) {
	initTestEditor();
	struct buffer *buf = make_test_buffer("scratch text");
	free(buf->filename);
	buf->filename = NULL;

	revert(); /* must not dereference NULL */

	TEST_ASSERT_NOT_NULL(E.buf);
	TEST_ASSERT_EQUAL_INT(1, E.buf->numrows);
	TEST_ASSERT_EQUAL_STRING("scratch text", row_str(E.buf, 0));

	cleanupTestEditor();
}

/* ---- B3: zap-to-char must not split a UTF-8 character.
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

/* ---- B4: revert() must refuse when the file no longer exists.
 *
 * editorOpen returns 0 both when it loaded a file and when the file is
 * missing (ENOENT -> posts "(New file)").  revert() only tested < 0,
 * so reverting a buffer whose file was never created silently replaced
 * it with an empty, clean buffer.  revert() ends in destroyBuffer(),
 * which frees the undo stack, so C-_ could not cross it and the work
 * was unrecoverable -- and because the replacement was clean, C-x C-c
 * exited without warning. */

void test_revert_missing_file_refuses(void) {
	initTestEditor();
	struct buffer *buf = make_test_buffer("typed but never saved");
	free(buf->filename);
	buf->filename = xstrdup("/tmp/emil_regress_definitely_absent");
	unlink("/tmp/emil_regress_definitely_absent");
	buf->dirty = 1;

	revert();

	/* Buffer must be untouched: same object, same contents. */
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_EQUAL_INT(1, E.buf->numrows);
	TEST_ASSERT_EQUAL_STRING("typed but never saved", row_str(E.buf, 0));
	TEST_ASSERT(E.buf->dirty != 0);

	cleanupTestEditor();
}

/* The case that must keep working: the file exists, the buffer has
 * unsaved edits, revert discards them and reloads from disk. */
void test_revert_existing_file_still_reloads(void) {
	initTestEditor();
	char *path = writeTempFile("revert", "on disk line\n");

	struct buffer *buf = make_test_buffer("UNSAVED EDIT");
	free(buf->filename);
	buf->filename = xstrdup(path);
	buf->dirty = 1;

	revert();

	TEST_ASSERT_EQUAL_INT(1, E.buf->numrows);
	TEST_ASSERT_EQUAL_STRING("on disk line", row_str(E.buf, 0));

	unlink(path);
	free(path);
	cleanupTestEditor();
}

/* ---- B5: reverse search must not move forward past point.
 *
 * The "match on the same row as the cursor" block was hardcoded
 * forward-only (strstr from cx + 1) and never consulted direction, so
 * C-r found the next match after point instead of the previous one. */

void test_reverse_search_goes_backward(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "aaa", "foo bar foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 5; /* between the 'f' at col 0 and the one at col 8 */

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	reverseFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx); /* was 9: forward past point */
	TEST_ASSERT(E.buf->cx < 5);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Stepping to an earlier row backwards should land on that row's LAST
 * match, otherwise repeated C-r skips every match but the first: the
 * same-row block finds nothing before the first match and steps back
 * another row. */
void test_reverse_search_previous_row_takes_last_match(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "foo bar foo", "aaa" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 0;

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	reverseFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(8, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Forward search must be untouched.  Note the first character of a
 * forward search scans from the top of the buffer, not from point --
 * that is this editor's existing behaviour and is deliberately pinned
 * here so the reverse fix can be seen not to have disturbed it. */
void test_forward_search_first_char_unchanged(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "aaa", "foo bar foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 5;

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* C-s repeat drives the forward same-row block, which is the branch
 * the reverse fix sits next to. */
void test_forward_search_repeat_advances(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "aaa", "foo bar foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 5;

	int keys[] = { 'f', CTRL('s'), '\r' };
	scriptKeys(keys, 3);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(8, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

void setUp(void) {
}

void tearDown(void) {
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_nested_prompt_preserves_buffer);
	RUN_TEST(test_double_nested_prompt_preserves_buffer);

	RUN_TEST(test_revert_null_filename_survives);

	RUN_TEST(test_zap_arrow_key_does_not_corrupt_utf8);
	RUN_TEST(test_zap_meta_key_does_not_corrupt_utf8);
	RUN_TEST(test_zap_ascii_still_works);

	RUN_TEST(test_revert_missing_file_refuses);
	RUN_TEST(test_revert_existing_file_still_reloads);

	RUN_TEST(test_reverse_search_goes_backward);
	RUN_TEST(test_reverse_search_previous_row_takes_last_match);
	RUN_TEST(test_forward_search_first_char_unchanged);
	RUN_TEST(test_forward_search_repeat_advances);

	return TEST_END();
}
