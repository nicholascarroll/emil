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

	return TEST_END();
}
