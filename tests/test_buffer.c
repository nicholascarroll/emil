/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_buffer.c: Row operations, screen cache, coordinate mapping.
 *
 * Killing a buffer, what a prompt hands back, and how command output
 * is split into rows are tested through the commands that do them.
 * The display helpers and the blob loader's flag cases are still
 * tested as module functions. */

#include "test.h"
#include "test_harness.h"
#include "prompt.h"
#include "buffer.h"
#include "keymap.h"
#include <fcntl.h>
#include <unistd.h>
#include "util.h"
#include "unicode.h"
#include <stdint.h>

/* ---- Kill-buffer confirmation ----
 *
 * Matches kill-buffer in Emacs: confirm only for a modified buffer
 * that is visiting a file.  A modified buffer with no filename is
 * scratch space and is killed without a prompt. */

static void typeString(const char *text) {
	for (; *text; text++) {
		E.self_insert_key = (unsigned char)*text;
		processKeypress(CMD_SELF_INSERT);
	}
}

/* C-x k with `answer` waiting; returns how many keys it read. */
static int killWithAnswer(int answer) {
	int keys[] = { answer };
	scriptKeys(keys, 1);
	muteStdout();
	processKeypress(CMD_KILL_BUFFER);
	unmuteStdout();
	int used = test_key_pos;
	clearKeys();
	return used;
}

/* Is there a buffer called `name`?  Is there one whose first row is
 * `text`?  Asked of the buffer list, not of a pointer that may have
 * been freed. */
static int bufferNamed(const char *name) {
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next)
		if (b->filename && strcmp(b->filename, name) == 0)
			return 1;
	return 0;
}

static int bufferHolding(const char *text) {
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next)
		if (b->numrows > 0 && strcmp(row_str(b, 0), text) == 0)
			return 1;
	return 0;
}

/* A temp file holding one line; its path in `path`. */
static void tempFile(char *path, size_t size, const char *line) {
	testTmpPath(path, size, "emil_kill_XXXXXX");
	int fd = mkstemp(path);
	TEST_ASSERT(fd >= 0);
	TEST_ASSERT_EQUAL_INT((int)strlen(line), (int)write(fd, line, strlen(line)));
	close(fd);
}

void test_kill_confirm_dirty_named_buffer(void) {
	char path[PATH_MAX];
	tempFile(path, sizeof(path), "saved\n");
	makeMinibuffer();
	make_test_buffer(NULL);
	struct buffer *buf = findFileTyping(path);
	typeString("unsaved work ");
	TEST_ASSERT_TRUE(buf->dirty);

	/* Asked; n keeps the buffer, edits and all. */
	TEST_ASSERT_EQUAL_INT(1, killWithAnswer('n'));
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_EQUAL_STRING("unsaved work saved", row_str(buf, 0));

	/* Asked again; y kills it. */
	TEST_ASSERT_EQUAL_INT(1, killWithAnswer('y'));
	TEST_ASSERT_FALSE(bufferHolding("unsaved work saved"));

	freeMinibuffer();
	unlink(path);
}

void test_kill_no_confirm_dirty_unnamed_buffer(void) {
	makeMinibuffer();
	struct buffer *other = make_test_buffer("other");
	struct buffer *buf = make_test_buffer(NULL);
	buf->next = other; /* keep both in the buffer list */
	typeString("scratch jottings");
	TEST_ASSERT_TRUE(buf->dirty);
	TEST_ASSERT_NULL(buf->filename);

	/* Not asked: the n that waits is never read. */
	TEST_ASSERT_EQUAL_INT(0, killWithAnswer('n'));
	TEST_ASSERT_FALSE(bufferHolding("scratch jottings"));
	freeMinibuffer();
}

void test_kill_no_confirm_clean_named_buffer(void) {
	char path[PATH_MAX];
	tempFile(path, sizeof(path), "saved\n");
	makeMinibuffer();
	make_test_buffer(NULL);
	struct buffer *buf = findFileTyping(path);
	TEST_ASSERT_FALSE(buf->dirty);
	char *name = xstrdup(buf->filename);

	TEST_ASSERT_EQUAL_INT(0, killWithAnswer('n'));
	TEST_ASSERT_FALSE(bufferNamed(name));

	free(name);
	freeMinibuffer();
	unlink(path);
}

/* A special buffer carries a name ("*scratch*", "*Completions*"), so
 * being special is what spares it the question, not the lack of a
 * name.  Killing the last buffer leaves a fresh *scratch*; typing in
 * it modifies it. */
void test_kill_no_confirm_special_buffer_with_name(void) {
	makeMinibuffer();
	make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, killWithAnswer('n'));
	TEST_ASSERT_EQUAL_STRING("*scratch*", E.buf->filename);

	typeString("completions");
	TEST_ASSERT_TRUE(E.buf->dirty);

	TEST_ASSERT_EQUAL_INT(0, killWithAnswer('n'));
	TEST_ASSERT_FALSE(bufferHolding("completions"));
	TEST_ASSERT_EQUAL_STRING("*scratch*", E.buf->filename);
	freeMinibuffer();
}

/* ---- The newline rule, in the sanitize build ---- */

#ifdef EMIL_DEBUG_ROW_BYTES
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
static void assert_row_write_aborts(int append) {
	pid_t pid = fork();
	TEST_ASSERT(pid >= 0);
	if (pid == 0) {
		if (freopen("/dev/null", "w", stderr) == NULL)
			_exit(1);
		struct buffer *buf = make_test_buffer("x");
		if (append)
			appendRowRaw(buf, (const uint8_t *)"a\nb", 3);
		else
			insertRow(buf, 0, (const uint8_t *)"a\nb", 3);
		_exit(0);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	TEST_ASSERT(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

void test_debug_check_stops_newline_entering_a_row(void) {
	assert_row_write_aborts(0);
	assert_row_write_aborts(1);
}
#endif

/* ---- Coordinate mapping ---- */

void test_chars_to_display_ascii(void) {
	struct buffer *buf = make_test_buffer("Hello");
	TEST_ASSERT_EQUAL_INT(0, charsToDisplayColumn(&buf->row[0], 0));
	TEST_ASSERT_EQUAL_INT(3, charsToDisplayColumn(&buf->row[0], 3));
	TEST_ASSERT_EQUAL_INT(5, charsToDisplayColumn(&buf->row[0], 5));
}

void test_chars_to_display_tab(void) {
	struct buffer *buf = make_test_buffer("\tA");
	TEST_ASSERT_EQUAL_INT(0, charsToDisplayColumn(&buf->row[0], 0));
	TEST_ASSERT_EQUAL_INT(8, charsToDisplayColumn(&buf->row[0], 1));
	TEST_ASSERT_EQUAL_INT(9, charsToDisplayColumn(&buf->row[0], 2));
}

void test_chars_to_display_control(void) {
	struct buffer *buf = make_test_buffer("\x01"
					      "A");
	TEST_ASSERT_EQUAL_INT(2, charsToDisplayColumn(&buf->row[0], 1));
	TEST_ASSERT_EQUAL_INT(3, charsToDisplayColumn(&buf->row[0], 2));
}

void test_chars_to_display_multibyte(void) {
	/* "A¢B" — ¢ is 2 bytes, 1 column */
	struct buffer *buf = make_test_buffer("A\xC2\xA2"
					      "B");
	TEST_ASSERT_EQUAL_INT(1, charsToDisplayColumn(&buf->row[0], 1));
	TEST_ASSERT_EQUAL_INT(2, charsToDisplayColumn(&buf->row[0], 3));
	TEST_ASSERT_EQUAL_INT(3, charsToDisplayColumn(&buf->row[0], 4));
}

void test_calculate_line_width(void) {
	struct buffer *buf = make_test_buffer("ABCDE");
	TEST_ASSERT_EQUAL_INT(5, calculateLineWidth(&buf->row[0]));
	/* Destroy before creating a new buffer to avoid orphaning this one */
	destroyBuffer(buf);
	E.headbuf = NULL;

	buf = make_test_buffer("\tX");
	TEST_ASSERT_EQUAL_INT(9, calculateLineWidth(&buf->row[1 - 1]));
}

/* ---- Screen line counting ---- */

/* A row exactly filling the window wraps once more: the end of the row
 * starts an empty screen line, like any other wrap point. */
void test_count_screen_lines_exact(void) {
	struct buffer *buf = make_test_buffer("1234567890");
	TEST_ASSERT_EQUAL_INT(2, countScreenLines(&buf->row[0], 10));
	TEST_ASSERT_EQUAL_INT(1, countScreenLines(&buf->row[0], 11));
}

void test_count_screen_lines_long(void) {
	struct buffer *buf = make_test_buffer("abcdefghijklmnopqrstuvwxy");
	int lines = countScreenLines(&buf->row[0], 10);
	TEST_ASSERT(lines >= 2);
}

void test_word_wrap_break(void) {
	struct buffer *buf = make_test_buffer("hello world");
	int break_col, break_byte;
	int more =
		wordWrapBreak(&buf->row[0], 7, 0, 0, &break_col, &break_byte);
	TEST_ASSERT_EQUAL_INT(1, more);
	TEST_ASSERT_EQUAL_INT(6, break_col);
	TEST_ASSERT_EQUAL_INT(6, break_byte);
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- What a prompt hands back ----
 *
 * The minibuffer holds rows like any other buffer; no row ever contains
 * a literal 0x0A.  A newline is typed at a prompt with C-q C-j, and
 * text crosses the boundary in two forms: joined with "\n" for the
 * command that asked, joined with "^J" for the screen. */

/* Answer a prompt with `text` ('\n' typed as C-q C-j) and RET.  The
 * replace prompt is used because it is one of the two that accept a
 * newline at all; the others refuse it with a message. */
static char *promptTyping(const char *text) {
	int keys[64];
	int n = 0;
	for (const char *c = text; *c && n < 60; c++) {
		if (*c == '\n')
			keys[n++] = CTRL('q');
		keys[n++] = (*c == '\n') ? CTRL('j') : (unsigned char)*c;
	}
	keys[n++] = '\r';
	makeMinibuffer();
	(void)make_test_buffer(NULL);
	scriptKeys(keys, n);
	muteStdout();
	char *got = (char *)editorPrompt("p: ", PROMPT_REPLACE, NULL);
	unmuteStdout();
	TEST_ASSERT_EQUAL_INT(n, test_key_pos);
	clearKeys();
	freeMinibuffer();
	return got;
}

static int frameHas(const char *want) {
	int n = (int)strlen(want);
	for (int i = 0; i + n <= E.render_buf.len; i++)
		if (memcmp(E.render_buf.b + i, want, n) == 0)
			return 1;
	return 0;
}

void test_minibuf_roundtrip_multiline(void) {
	char *got = promptTyping("foo\nbar");
	TEST_ASSERT_NOT_NULL(got);
	if (got)
		TEST_ASSERT_EQUAL_STRING("foo\nbar", got);
	free(got);
}

/* While the prompt waits, a row break shows as ^J. */
void test_minibuf_display_uses_caret(void) {
	char *got = promptTyping("a\nb\nc");
	free(got);
	TEST_ASSERT(frameHas("a^Jb^Jc"));
}

/* Typing only C-q C-j hands back "\n".  This is the line-join entry;
 * the prompt must not take it for an empty answer. */
void test_minibuf_bare_newline_is_not_empty(void) {
	char *got = promptTyping("\n");
	TEST_ASSERT_NOT_NULL(got);
	if (got)
		TEST_ASSERT_EQUAL_STRING("\n", got);
	free(got);
}

/* Text for a prompt or the status line is drawn raw, so control
 * bytes become caret notation, C1 controls '?', invalid bytes U+FFFD,
 * and a full output buffer never ends inside a character. */
void test_utf8_sanitize_line(void) {
	char out[32];
	const struct {
		const char *in, *want;
	} cases[] = {
		{ "a\nb", "a^Jb" },
		{ "\t\x1b[2J\x7f", "^I^[[2J^?" },
		{ "\xc2\x9b", "?" },		    /* U+009B, 8-bit CSI */
		{ "x\xff", "x\xef\xbf\xbd" },	    /* invalid byte */
		{ "\xe0\xb8\x81", "\xe0\xb8\x81" }, /* Thai passes through */
		{ "", "" },
	};
	for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
		size_t len = strlen(cases[i].in);
		TEST_ASSERT_EQUAL_UINT(
			len, utf8SanitizeLine((const uint8_t *)cases[i].in, len,
					      out, sizeof(out)));
		TEST_ASSERT_EQUAL_STRING(cases[i].want, out);
	}
	/* Room for "ab" and part of U+0E01: stop before it. */
	TEST_ASSERT_EQUAL_UINT(2, utf8SanitizeLine((const uint8_t *)"ab\xe0"
								    "\xb8\x81",
						   5, out, 4));
	TEST_ASSERT_EQUAL_STRING("ab", out);
}

void test_minibuf_trailing_newline(void) {
	/* A pattern ending in a row break keeps it. */
	char *got = promptTyping("x\n");
	TEST_ASSERT_NOT_NULL(got);
	if (got)
		TEST_ASSERT_EQUAL_STRING("x\n", got);
	free(got);
}

/* ================================================================
 * bufferLoadBlob (#117 R2)
 *
 * Five places split a blob into rows and disagreed about the two edge
 * cases that matter: what happens to a trailing segment with no
 * newline after it, and whether CR is stripped.  These pin the single
 * answer.
 * ================================================================ */

static struct buffer *blobBuf(void) {
	struct buffer *b = newBuffer();
	bufferResetRows(b);
	return b;
}

static void assertRow(struct buffer *b, int i, const char *want) {
	TEST_ASSERT_TRUE(i < b->numrows);
	if (i >= b->numrows)
		return;
	TEST_ASSERT_EQUAL_INT((int)strlen(want), b->row[i].size);
	TEST_ASSERT_EQUAL_INT(0, memcmp(b->row[i].chars, want, strlen(want)));
}

#ifndef EMIL_DISABLE_SHELL
/* M-! command RET; returns the buffer the output is shown in. */
static struct buffer *shellOutput(const char *command) {
	int keys[128];
	int n = 0;
	for (const char *c = command; *c && n < 126; c++)
		keys[n++] = (unsigned char)*c;
	keys[n++] = '\r';
	makeMinibuffer();
	(void)make_test_buffer(NULL);
	scriptKeys(keys, n);
	muteStdout();
	processKeypress(CMD_SHELL_CMD);
	unmuteStdout();
	clearKeys();
	freeMinibuffer();
	TEST_ASSERT_NOT_NULL(E.buf->filename);
	TEST_ASSERT_EQUAL_STRING("*Shell Output*", E.buf->filename);
	return E.buf;
}

/* DEF-4: the *Diff* copy emitted the last row before counting its
 * final byte, so "alpha\nbravo" captured as "alpha" / "brav".  The
 * same split of command output into rows serves M-!. */
void test_blob_no_trailing_newline_keeps_last_byte(void) {
	struct buffer *b = shellOutput("printf 'alpha\\nbravo'");
	TEST_ASSERT_EQUAL_INT(2, b->numrows);
	assertRow(b, 0, "alpha");
	assertRow(b, 1, "bravo");
}
#endif /* EMIL_DISABLE_SHELL */

void test_blob_empty(void) {
	struct buffer *b = blobBuf();
	bufferLoadBlob(b, (const uint8_t *)"", 0, 0);
	/* Never rowless (§4.9). */
	TEST_ASSERT_EQUAL_INT(1, b->numrows);
	assertRow(b, 0, "");
	destroyBuffer(b);
}

void test_blob_crlf_flag(void) {
	struct buffer *b = blobBuf();
	bufferLoadBlob(b, (const uint8_t *)"alpha\r\nbravo\r\n", 14, BLOB_CRLF);
	TEST_ASSERT_EQUAL_INT(2, b->numrows);
	assertRow(b, 0, "alpha");
	assertRow(b, 1, "bravo");
	destroyBuffer(b);

	/* Without the flag the CR is ordinary content, and stays. */
	b = blobBuf();
	bufferLoadBlob(b, (const uint8_t *)"alpha\r\n", 7, 0);
	TEST_ASSERT_EQUAL_INT(1, b->numrows);
	assertRow(b, 0, "alpha\r");
	destroyBuffer(b);

	/* A lone CR mid-line is never touched (§3.21.1). */
	b = blobBuf();
	bufferLoadBlob(b, (const uint8_t *)"a\rb\n", 4, BLOB_CRLF);
	TEST_ASSERT_EQUAL_INT(1, b->numrows);
	assertRow(b, 0, "a\rb");
	destroyBuffer(b);
}

#ifndef EMIL_DISABLE_SHELL
void test_blob_blank_lines_preserved(void) {
	struct buffer *b = shellOutput("printf 'a\\n\\nb\\n'");
	TEST_ASSERT_EQUAL_INT(3, b->numrows);
	assertRow(b, 0, "a");
	assertRow(b, 1, "");
	assertRow(b, 2, "b");
}
#endif /* EMIL_DISABLE_SHELL */

/* Appends rather than resets: register.c writes a header first. */
void test_blob_appends_beneath_existing_rows(void) {
	struct buffer *b = blobBuf();
	insertRow(b, 0, (const uint8_t *)"header", 6);
	bufferLoadBlob(b, (const uint8_t *)"one\ntwo\n", 8, 0);
	TEST_ASSERT_EQUAL_INT(3, b->numrows);
	assertRow(b, 0, "header");
	assertRow(b, 1, "one");
	assertRow(b, 2, "two");
	destroyBuffer(b);
}

/* Bulk population must not mark the buffer dirty: §3.21.1 relies on
 * that so an unedited file is never rewritten merely for being
 * opened. */
void test_blob_does_not_mark_dirty(void) {
	struct buffer *b = blobBuf();
	b->dirty = 0;
	bufferLoadBlob(b, (const uint8_t *)"alpha\n", 6, 0);
	TEST_ASSERT_EQUAL_INT(0, b->dirty);
	destroyBuffer(b);
}

int main(void) {
	TEST_BEGIN();

	/* While a shell command runs the editor watches the terminal,
	 * fd 0, for C-g.  Give the suite an fd 0 with nothing on it. */
	int devnull = open("/dev/null", O_RDONLY);
	if (devnull >= 0) {
		dup2(devnull, STDIN_FILENO);
		if (devnull != STDIN_FILENO)
			close(devnull);
	}

	#ifdef EMIL_DEBUG_ROW_BYTES
	RUN_TEST(test_debug_check_stops_newline_entering_a_row);
#endif

	RUN_TEST(test_chars_to_display_ascii);
	RUN_TEST(test_chars_to_display_tab);
	RUN_TEST(test_chars_to_display_control);
	RUN_TEST(test_chars_to_display_multibyte);
	RUN_TEST(test_calculate_line_width);

	RUN_TEST(test_count_screen_lines_exact);
	RUN_TEST(test_count_screen_lines_long);
	RUN_TEST(test_word_wrap_break);

	/* Minibuffer serialization */
	RUN_TEST(test_minibuf_roundtrip_multiline);
	RUN_TEST(test_minibuf_display_uses_caret);
	RUN_TEST(test_minibuf_trailing_newline);
	RUN_TEST(test_minibuf_bare_newline_is_not_empty);
	RUN_TEST(test_utf8_sanitize_line);

	/* bufferLoadBlob (#117 R2) */
#ifndef EMIL_DISABLE_SHELL
	RUN_TEST(test_blob_no_trailing_newline_keeps_last_byte);
#endif
	RUN_TEST(test_blob_empty);
	RUN_TEST(test_blob_crlf_flag);
#ifndef EMIL_DISABLE_SHELL
	RUN_TEST(test_blob_blank_lines_preserved);
#endif
	RUN_TEST(test_blob_appends_beneath_existing_rows);
	RUN_TEST(test_blob_does_not_mark_dirty);


	/* Kill-buffer confirmation */
	RUN_TEST(test_kill_confirm_dirty_named_buffer);
	RUN_TEST(test_kill_no_confirm_dirty_unnamed_buffer);
	RUN_TEST(test_kill_no_confirm_clean_named_buffer);
	RUN_TEST(test_kill_no_confirm_special_buffer_with_name);

	return TEST_END();
}
