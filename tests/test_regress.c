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
#include "display.h"
#include "region.h"
#include "history.h"
#include "abuf.h"
#include <string.h>
#include <limits.h>
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

/* Portable substring search over a byte range (the haystack contains
 * escape sequences, not a NUL-terminated string, and memmem is a GNU
 * extension). */
static int containsBytes(const char *hay, int hay_len, const char *needle,
			 int needle_len) {
	for (int i = 0; i + needle_len <= hay_len; i++) {
		if (memcmp(hay + i, needle, needle_len) == 0)
			return 1;
	}
	return 0;
}

/* Render a window's text area and return the raw bytes (escapes kept,
 * because the region highlight IS an escape sequence).  Caller frees. */
static char *render_rows(struct window *win, int *len_out) {
	struct abuf ab = ABUF_INIT;
	drawRows(win, &ab, E.screenrows, E.screencols);
	char *out = xmalloc(ab.len + 1);
	memcpy(out, ab.b, ab.len);
	out[ab.len] = '\0';
	if (len_out)
		*len_out = ab.len;
	abFree(&ab);
	return out;
}

/* Render the status bar for a window.  Caller frees. */
static char *render_status(struct window *win, int *len_out) {
	struct abuf ab = ABUF_INIT;
	drawStatusBar(win, &ab, 1);
	char *out = xmalloc(ab.len + 1);
	memcpy(out, ab.b, ab.len);
	out[ab.len] = '\0';
	if (len_out)
		*len_out = ab.len;
	abFree(&ab);
	return out;
}

/* Strip ANSI escapes, leaving visible bytes (multi-byte UTF-8 intact). */
static char *strip_escapes(const char *in, int len, int *out_len) {
	char *out = xmalloc(len + 1);
	int oi = 0, i = 0;
	while (i < len) {
		if (in[i] == '\x1b') {
			i++;
			if (i < len && in[i] == '[') {
				i++;
				while (i < len &&
				       !((in[i] >= 'A' && in[i] <= 'Z') ||
					 (in[i] >= 'a' && in[i] <= 'z')))
					i++;
				if (i < len)
					i++;
			}
		} else if (in[i] == '\r' || in[i] == '\n') {
			i++;
		} else {
			out[oi++] = in[i++];
		}
	}
	out[oi] = '\0';
	if (out_len)
		*out_len = oi;
	return out;
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

/* Forward search on the cursor's own row starts at the cursor column.
 *
 * This test previously asserted cx == 0 and carried a comment saying
 * that a forward search scans from the top of the buffer rather than
 * from point, pinning that as intended behaviour.  It was not
 * intended: it is B14, and the assertion is now the Emacs one.  With
 * the cursor at column 5 of "foo bar foo", the match before point at
 * column 0 must be skipped in favour of the one at column 8. */
void test_forward_search_first_char_from_point(void) {
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
	TEST_ASSERT_EQUAL_INT(8, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* C-s repeat drives the forward same-row block, which is the branch
 * the reverse fix sits next to.  The cursor starts at column 0 so that
 * the fresh search matches at point (column 0) and the repeat then has
 * somewhere to advance to (column 8); starting at column 5 would make
 * the fresh search land on column 8 already and the repeat would
 * wrap, which is a different code path. */
void test_forward_search_repeat_advances(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "aaa", "foo bar foo" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 0;

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


/* ---- B14: forward search must start from point, not the top of the
 * buffer.
 *
 * Reported against the released build: C-s always began scanning at
 * row 0.  The seeding line read
 *
 *     current = (direction == -1) ? bufr->cy : -1;
 *
 * so a fresh backward search started on the cursor's row but a fresh
 * forward one started at -1, and the row-stepping loop below then
 * began at row 0.  Emacs searches forward from point and only wraps
 * to the top after passing the end of the buffer. */

void test_forward_search_starts_from_point(void) {
	initTestEditor();
	makeMinibuffer();
	/* "foo" appears both above and below the cursor. */
	const char *lines[] = { "foo above", "bar", "foo below" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->cy = 1;
	buf->cx = 0;

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	/* Must find the occurrence *below* point, not the one above. */
	TEST_ASSERT_EQUAL_INT(2, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* A match beginning exactly at point is a valid first match: Emacs
 * C-s finds the occurrence under the cursor rather than skipping it. */
void test_forward_search_matches_at_point(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "foo above", "foo at point" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 0;

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

/* Within a row, a fresh forward search starts at the cursor column. */
void test_forward_search_starts_from_column(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *buf = make_test_buffer("foo bar foo");
	buf->cy = 0;
	buf->cx = 5;

	int keys[] = { 'f', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(8, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Having passed the end of the buffer with no match, the search wraps
 * to the top -- an occurrence above point is still reachable. */
void test_forward_search_wraps_to_find_earlier_match(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "target here", "zzz", "zzz" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->cy = 1;
	buf->cx = 0;

	int keys[] = { 't', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Deleting a character in the minibuffer re-runs the search from the
 * origin, not from wherever the longer query had landed the cursor.
 * "a" first occurs at column 1, "ab" only at column 4; after
 * backspacing the search must return to column 1. */
void test_forward_search_backspace_returns_to_origin(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *buf = make_test_buffer("xa yab");
	buf->cy = 0;
	buf->cx = 0;

	int keys[] = { 'a', 'b', KEY_BACKSPACE, '\r' };
	scriptKeys(keys, 4);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(1, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* ================= B6 — wrong-buffer mark check ==================
 *
 * computeRowHighlightBounds(buf, ...) reads everything from its `buf`
 * parameter except one call: markInvalidSilent(), which takes no
 * argument and consults the global E.buf.  So an unfocused window's
 * selection is drawn or not drawn according to whether the *focused*
 * buffer happens to have a valid mark.
 *
 * Both tests give buffer B the identical, valid, active selection.
 * Only the focused buffer A's mark differs between them, and B must
 * render the same way in both. */

static void b6_setup(struct buffer **a_out, struct buffer **b_out) {
	initTestEditor();

	/* Second window, showing buffer B, unfocused. */
	E.windows = realloc(E.windows, 2 * sizeof(struct window *));
	E.windows[1] = calloc(1, sizeof(struct window));
	E.nwindows = 2;
	E.windows[0]->focused = 1;
	E.windows[1]->focused = 0;
	E.windows[0]->height = 10;
	E.windows[1]->height = 10;

	struct buffer *a = make_test_buffer("aaaa focused buffer");

	struct buffer *b = newBuffer();
	insertRow(b, 0, (const uint8_t *)"bbbb selected text", 18);
	b->cx = 0;
	b->cy = 0;
	b->dirty = 0;
	clearUndosAndRedos(b);
	/* A valid, active, non-empty region on B. */
	b->markx = 10;
	b->marky = 0;
	b->mark_active = 1;

	b->next = E.headbuf;
	E.headbuf = b;

	E.windows[1]->buf = b;
	E.buf = a;
	E.windows[0]->buf = a;

	*a_out = a;
	*b_out = b;
}

/* Focused buffer A has NO mark.  B's own selection is valid, so B must
 * still be highlighted. */
void test_b6_unfocused_selection_drawn_when_focused_has_no_mark(void) {
	struct buffer *a, *b;
	b6_setup(&a, &b);

	a->markx = -1;
	a->marky = -1;
	a->mark_active = 0;

	int len = 0;
	char *out = render_rows(E.windows[1], &len);
	int highlighted = containsBytes(out, len, "\x1b[7m", 4);
	free(out);

	TEST_ASSERT(highlighted);

	cleanupTestEditor();
}

/* Control: same B, but now A does have a valid mark.  This one passes
 * today — the pair is what shows the rendering depends on A. */
void test_b6_unfocused_selection_drawn_when_focused_has_mark(void) {
	struct buffer *a, *b;
	b6_setup(&a, &b);

	a->cx = 0;
	a->cy = 0;
	a->markx = 4;
	a->marky = 0;
	a->mark_active = 1;

	int len = 0;
	char *out = render_rows(E.windows[1], &len);
	int highlighted = containsBytes(out, len, "\x1b[7m", 4);
	free(out);

	TEST_ASSERT(highlighted);

	cleanupTestEditor();
}

/* ================= B7 — plain Shift-Tab does nothing ==============
 *
 * unindent(int rept) loops `for (i = 0; i < rept; i++)`, and
 * CMD_UNINDENT passes E.uarg raw, which is 0 with no prefix argument.
 * Every other command routes through UARG_COUNT(), which maps 0 -> 1. */

void test_b7_unindent_with_no_prefix_removes_one_tab(void) {
	initTestEditor();
	struct buffer *buf = make_test_buffer("\t\t\tcode");
	buf->cy = 0;
	buf->cx = 3;

	unindent(0); /* what CMD_UNINDENT passes for a bare Shift-Tab */

	TEST_ASSERT_EQUAL_STRING("\t\tcode", row_str(E.buf, 0));

	cleanupTestEditor();
}

/* Explicit counts already work; pinned so a fix doesn't break them. */
void test_b7_unindent_with_explicit_count_unchanged(void) {
	initTestEditor();
	struct buffer *buf = make_test_buffer("\t\t\tcode");
	buf->cy = 0;
	buf->cx = 3;

	unindent(2);

	TEST_ASSERT_EQUAL_STRING("\tcode", row_str(E.buf, 0));

	cleanupTestEditor();
}

/* ================= B8 — Down in a prompt destroys typed text =======
 *
 * With history_pos == -1 (the user has not browsed history at all),
 * Down falls into the else branch, leaves history_pos at -1, and hits
 * replaceMinibufferText(E.minibuf, "").  The typed text is gone.
 *
 * Note the history must be non-empty for the bug to fire: the whole
 * block is guarded by `hist->count > 0`. */

void test_b8_down_without_browsing_keeps_typed_text(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *file = make_test_buffer("contents");
	addHistory(&E.file_history, "/tmp/previously-visited");

	int keys[] = { '/', 't', 'm', 'p', '/', 'a', 'b', 'c',
		       KEY_ARROW_DOWN, '\r' };
	scriptKeys(keys, 10);

	muteStdout();
	uint8_t *r = editorPrompt(file, "Find File: ", PROMPT_FILES, NULL);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_NOT_NULL(r);
	if (r)
		TEST_ASSERT_EQUAL_STRING("/tmp/abc", (const char *)r);

	free(r);
	freeMinibuffer();
	cleanupTestEditor();
}

/* The Emacs behaviour of the same key pair: Up parks the in-progress
 * text, Down brings it back.  This is the "full fix" option (decision
 * 2 in the audit); the minimal fix only makes the test above pass. */
void test_b8_up_then_down_restores_typed_text(void) {
	initTestEditor();
	makeMinibuffer();
	struct buffer *file = make_test_buffer("contents");
	addHistory(&E.file_history, "/tmp/previously-visited");

	int keys[] = { '/', 't', 'm', 'p', '/', 'a', 'b', 'c',
		       KEY_ARROW_UP, KEY_ARROW_DOWN, '\r' };
	scriptKeys(keys, 11);

	muteStdout();
	uint8_t *r = editorPrompt(file, "Find File: ", PROMPT_FILES, NULL);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_NOT_NULL(r);
	if (r)
		TEST_ASSERT_EQUAL_STRING("/tmp/abc", (const char *)r);

	free(r);
	freeMinibuffer();
	cleanupTestEditor();
}

/* ================= B9 — absolutePath yields "//foo" at cwd "/" =====
 *
 * cleanPath keeps the empty leading segment produced by
 * "/" + "/" + name, so at cwd "/" the same file resolves to two
 * different strings depending on how it was named, and
 * findBufferByName's comparison stops deduplicating. */

void test_b9_cleanpath_collapses_double_leading_slash(void) {
	initTestEditor();
	char path[64];
	snprintf(path, sizeof(path), "//foo");

	cleanPath(path);

	TEST_ASSERT_EQUAL_STRING("/foo", path);

	cleanupTestEditor();
}

void test_b9_absolutepath_at_root_matches_absolute_form(void) {
	initTestEditor();
	char oldcwd[PATH_MAX];
	if (getcwd(oldcwd, sizeof(oldcwd)) == NULL) {
		TEST_ASSERT(0);
		cleanupTestEditor();
		return;
	}
	if (chdir("/") != 0) {
		TEST_ASSERT(0);
		cleanupTestEditor();
		return;
	}

	char *relative = absolutePath("rootfile.txt");
	char *absolute = absolutePath("/rootfile.txt");

	TEST_ASSERT_EQUAL_STRING("/rootfile.txt", relative);
	TEST_ASSERT_EQUAL_STRING(absolute, relative);

	free(relative);
	free(absolute);
	if (chdir(oldcwd) != 0)
		TEST_ASSERT(0);
	cleanupTestEditor();
}

/* ================= B10 — whatCursor reads window 0 =================
 *
 * `int screen_y = E.buf->cy - E.windows[0]->rowoff + 1;` — hardcoded
 * to the first window, so C-x = reports the wrong screen row whenever
 * focus is anywhere else. */

void test_b10_whatcursor_uses_focused_window_rowoff(void) {
	initTestEditor();

	E.windows = realloc(E.windows, 2 * sizeof(struct window *));
	E.windows[1] = calloc(1, sizeof(struct window));
	E.nwindows = 2;
	E.windows[0]->focused = 0;
	E.windows[0]->rowoff = 0;
	E.windows[1]->focused = 1;
	E.windows[1]->rowoff = 10;

	const char *lines[20];
	for (int i = 0; i < 20; i++)
		lines[i] = "some line of text";
	struct buffer *buf = make_test_buffer_lines(lines, 20);
	E.windows[1]->buf = buf;
	buf->cy = 12;
	buf->cx = 0;

	whatCursor();

	/* Focused window starts at row 10, so cy 12 is screen row 3. */
	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "screen:3,"));

	cleanupTestEditor();
}

/* ================= B12 — statusLeft truncates mid-character ========
 *
 * `snprintf(trunc, ..., "...%s", dname + dlen - tail)` is byte
 * arithmetic against a *column* budget, so the tail pointer can land
 * inside a multi-byte sequence and emit invalid UTF-8 to the terminal.
 * truncateToCols() exists a few lines up and does this correctly;
 * statusRight uses it, statusLeft does not. */

void test_b12_statusleft_truncation_stays_valid_utf8(void) {
	initTestEditor();
	struct buffer *buf = make_test_buffer("content");

	/* A long CJK name: every character is 3 bytes, so a byte-based
	 * left-truncation lands mid-sequence for most widths. */
	free(buf->filename);
	buf->filename = xstrdup("/"
				"\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
				"\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
				"\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
				"\xE8\xAF\xAD\xE8\xAF\xAD\xE8\xAF\xAD"
				".txt");

	/* Narrow enough to force the truncation branch. */
	E.screencols = 40;

	int len = 0;
	char *raw = render_status(E.windows[0], &len);
	int vis_len = 0;
	char *vis = strip_escapes(raw, len, &vis_len);

	TEST_ASSERT_EQUAL_INT(1, utf8_validate((const uint8_t *)vis,
					       vis_len));

	free(vis);
	free(raw);
	cleanupTestEditor();
}

/* ================= B13 — statusLeft returns snprintf's length ======
 *
 * `left` is char[512]; statusLeft returns snprintf's would-be length,
 * and the caller does abAppend(ab, left, left_len).  With screencols
 * over ~508 and a long name, that reads past the end of the stack
 * buffer.  Latent, but one line to clamp.
 *
 * The read is out of bounds rather than wrong-valued, so the reliable
 * signal is a sanitizer; without one, the observable symptom is a
 * status bar longer than the screen. */

void test_b13_statusleft_does_not_overrun_its_buffer(void) {
	initTestEditor();
	struct buffer *buf = make_test_buffer("content");

	/* The name has to be longer than statusLeft's 512-byte output
	 * buffer but still SHORTER than name_width, or the "..." branch
	 * truncates it and nothing overflows.  550 bytes with a 600
	 * column screen sits in that window: name_width works out to
	 * 566, so no truncation, and snprintf returns 554 against a
	 * cap of 512.  No '/' in the name, so min_name == dlen. */
	char longname[551];
	memset(longname, 'x', sizeof(longname) - 1);
	longname[sizeof(longname) - 1] = '\0';
	free(buf->filename);
	buf->filename = xstrdup(longname);

	E.screencols = 600;

	int len = 0;
	char *raw = render_status(E.windows[0], &len);

	/* snprintf NUL-terminates at 511; returning its would-be length
	 * makes the caller append the terminator and then whatever
	 * happened to be on the stack after it.  A status bar never
	 * legitimately contains a NUL byte, so this is a reliable
	 * signal without needing a sanitizer -- though ASan also
	 * reports the read as a stack-buffer-overflow. */
	TEST_ASSERT_NULL(memchr(raw, '\0', len));

	free(raw);
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
	RUN_TEST(test_forward_search_first_char_from_point);
	RUN_TEST(test_forward_search_repeat_advances);

	RUN_TEST(test_forward_search_starts_from_point);
	RUN_TEST(test_forward_search_matches_at_point);
	RUN_TEST(test_forward_search_starts_from_column);
	RUN_TEST(test_forward_search_wraps_to_find_earlier_match);
	RUN_TEST(test_forward_search_backspace_returns_to_origin);


	RUN_TEST(test_b6_unfocused_selection_drawn_when_focused_has_no_mark);
	RUN_TEST(test_b6_unfocused_selection_drawn_when_focused_has_mark);
	RUN_TEST(test_b7_unindent_with_no_prefix_removes_one_tab);
	RUN_TEST(test_b7_unindent_with_explicit_count_unchanged);
	RUN_TEST(test_b8_down_without_browsing_keeps_typed_text);
	RUN_TEST(test_b8_up_then_down_restores_typed_text);
	RUN_TEST(test_b9_cleanpath_collapses_double_leading_slash);
	RUN_TEST(test_b9_absolutepath_at_root_matches_absolute_form);
	RUN_TEST(test_b10_whatcursor_uses_focused_window_rowoff);
	RUN_TEST(test_b12_statusleft_truncation_stays_valid_utf8);
	RUN_TEST(test_b13_statusleft_does_not_overrun_its_buffer);

	return TEST_END();
}
