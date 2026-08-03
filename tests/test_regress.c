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
#include "mutate.h"
#include "undo.h"
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
	TEST_ASSERT_EQUAL_INT(2, E.buf->numrows);
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
	TEST_ASSERT_EQUAL_INT(2, E.buf->numrows);
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

	TEST_ASSERT_EQUAL_INT(2, E.buf->numrows);
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

/* A forward search that finds nothing between point and the end of the
 * buffer FAILS THERE.  It does not quietly continue round the top and
 * land on a match above the starting point -- doing that moves point
 * backwards, which is what C-s must never do on its own.
 *
 * This test previously asserted the opposite, on the mistaken view
 * that wrapping was part of searching from point.  Emacs reports
 * "Failing I-search" and leaves point alone; only a further C-s wraps,
 * which is the next test. */
void test_forward_search_does_not_wrap_on_first_pass(void) {
	initTestEditor();
	makeMinibuffer();
	/* "target" exists only ABOVE the cursor. */
	const char *lines[] = { "target here", "zzz", "zzz" };
	struct buffer *lbuf = make_test_buffer_lines(lines, 3);
	lbuf->cy = 1;
	lbuf->cx = 0;

	int keys[] = { 't', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	/* Point unmoved, and nothing highlighted as a match. */
	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);
	TEST_ASSERT_EQUAL_INT(0, E.buf->match);

	freeMinibuffer();
	cleanupTestEditor();
}

/* A second C-s after a failing search is Emacs's wrap gesture: now the
 * occurrence above point is reachable. */
void test_forward_search_repeat_wraps_after_failing(void) {
	initTestEditor();
	makeMinibuffer();
	const char *lines[] = { "target here", "zzz", "zzz" };
	struct buffer *lbuf = make_test_buffer_lines(lines, 3);
	lbuf->cy = 1;
	lbuf->cx = 0;

	int keys[] = { 't', CTRL('s'), '\r' };
	scriptKeys(keys, 3);
	muteStdout();
	editorFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);

	freeMinibuffer();
	cleanupTestEditor();
}

/* Backward search is symmetric: nothing above point means the pass
 * fails rather than continuing round the bottom. */
void test_reverse_search_does_not_wrap_on_first_pass(void) {
	initTestEditor();
	makeMinibuffer();
	/* "target" exists only BELOW the cursor. */
	const char *lines[] = { "zzz", "zzz", "target here" };
	struct buffer *lbuf = make_test_buffer_lines(lines, 3);
	lbuf->cy = 1;
	lbuf->cx = 0;

	int keys[] = { 't', '\r' };
	scriptKeys(keys, 2);
	muteStdout();
	reverseFind();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_EQUAL_INT(0, E.buf->cx);
	TEST_ASSERT_EQUAL_INT(0, E.buf->match);

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


/* ------------------------------------------------------------------
 * #102 — undo records for insertions at the virtual EOF line.
 *
 * A buffer of N rows represents the byte string
 *
 *     T = row[0] "\n" row[1] "\n" ... row[N-1] "\n"
 *
 * so it cannot represent text that does not end in "\n".  Inserting D
 * at the virtual EOF line (0, N) therefore really changes len(D) + 1
 * bytes.  Every insertion path used to record only len(D), so undo left
 * the materialising newline behind (M-> RET C-_ grew the buffer by a
 * blank line, permanently, and silently marked the buffer clean) and
 * redo re-inserted it (one RET redone as two).
 *
 * The records are now anchored to the end of the last real row, which
 * states the whole change.  The invariant these tests pin is
 *
 *     bulkInsert(B, R) == A     and     bulkDelete(A, R) == B
 *
 * checked on buffer CONTENT rather than row counts, since row counts
 * are what the old records got wrong.
 * ------------------------------------------------------------------ */

/* The byte string the buffer would be written out as.  rowsToString
 * returns a length-counted block, not a C string, so terminate it —
 * these comparisons are on content, and a stray tail would make them
 * pass or fail for the wrong reason. */
/* The byte string the buffer would be written out as -- that is, with
 * the save policy applied.  rowsToString alone gives the buffer's own
 * content, which since #105 may lack a final newline; save() appends
 * one via bufferEnsureFinalNewline.  These tests are about the text a
 * user ends up with on disk, so mirror that here.  Done by copy rather
 * than by calling bufferEnsureFinalNewline, which modifies the buffer
 * and would disturb the undo/redo sequence being measured. */
static char *contentOf(struct buffer *buf) {
	size_t len;
	char *raw = rowsToString(buf, &len);
	int needs_nl = (len > 0 && raw[len - 1] != '\n');
	char *out = xmalloc(len + (size_t)needs_nl + 1);
	memcpy(out, raw, len);
	if (needs_nl)
		out[len++] = '\n';
	out[len] = '\0';
	free(raw);
	return out;
}

static void undoAll(struct buffer *buf) {
	for (int i = 0; i < 64 && buf->undo != NULL; i++)
		processKeypress(CMD_UNDO);
}

static void redoAll(struct buffer *buf) {
	for (int i = 0; i < 64 && buf->redo != NULL; i++)
		processKeypress(CMD_REDO);
}

static struct buffer *eofTestBuffer(void) {
	static const char *lines[2] = { "alpha", "beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	return buf;
}

/* Put the cursor on the virtual EOF line, the way M-> does. */
/* Put the cursor at the end of the buffer, the way M-> does.  Under
 * cy < numrows that is the end of the last real row, not a virtual
 * line past it.  For a newline-terminated buffer the last row is the
 * empty one, so cx is still 0 -- the same position in the text as
 * before, now nameable by the row array. */
static void gotoVirtualEOF(struct buffer *buf) {
	processKeypress(CMD_END_OF_FILE);
	TEST_ASSERT_EQUAL(buf->numrows - 1, buf->cy);
	TEST_ASSERT_EQUAL(buf->row[buf->cy].size, buf->cx);
}

static void setKillText(const char *s) {
	clearText(&E.kill);
	E.kill.str = (uint8_t *)xstrdup(s);
}

/* Drive one insertion path.  Kept as an enum rather than function
 * pointers so a failure names the path. */
enum eof_op {
	OP_RET,
	OP_OPEN_LINE,
	OP_NEWLINE_INDENT,
	OP_SELF_INSERT,
	OP_TAB,
	OP_YANK
};

static void runEofOp(enum eof_op op) {
	switch (op) {
	case OP_RET:
		processKeypress(CMD_NEWLINE);
		break;
	case OP_OPEN_LINE:
		processKeypress(CMD_OPEN_LINE);
		break;
	case OP_NEWLINE_INDENT:
		processKeypress(CMD_NEWLINE_INDENT);
		break;
	case OP_SELF_INSERT:
		E.self_insert_key = 'x';
		processKeypress(CMD_SELF_INSERT);
		break;
	case OP_TAB:
		processKeypress(CMD_TAB);
		break;
	case OP_YANK:
		setKillText("hello");
		processKeypress(CMD_YANK);
		break;
	}
}

/* §9.1 / §9.9 — round-trip matrix.  Each insertion path at the virtual
 * EOF, and the same path at the end of the last real line as a control:
 * undo restores the original, redo restores the edit, undo again
 * restores the original. */
static void eofRoundTrip(enum eof_op op, int at_virtual_eof) {
	initTestEditor();
	struct buffer *buf = eofTestBuffer();

	if (at_virtual_eof) {
		gotoVirtualEOF(buf);
	} else {
		buf->cy = buf->numrows - 1;
		buf->cx = buf->row[buf->cy].size;
	}

	char *original = contentOf(buf);
	runEofOp(op);
	char *edited = contentOf(buf);

	/* The operation must actually have done something, or the
	 * round-trip below would pass vacuously. */
	TEST_ASSERT(strcmp(original, edited) != 0);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	redoAll(buf);
	char *redone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(edited, redone);

	undoAll(buf);
	char *undone2 = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone2);

	free(original);
	free(edited);
	free(undone);
	free(redone);
	free(undone2);
	clearText(&E.kill);
	cleanupTestEditor();
}

void test_eof_roundtrip_newline(void) {
	eofRoundTrip(OP_RET, 1);
}
void test_eof_roundtrip_open_line(void) {
	eofRoundTrip(OP_OPEN_LINE, 1);
}
void test_eof_roundtrip_newline_indent(void) {
	eofRoundTrip(OP_NEWLINE_INDENT, 1);
}
void test_eof_roundtrip_self_insert(void) {
	eofRoundTrip(OP_SELF_INSERT, 1);
}
void test_eof_roundtrip_tab(void) {
	eofRoundTrip(OP_TAB, 1);
}
void test_eof_roundtrip_yank(void) {
	eofRoundTrip(OP_YANK, 1);
}

/* §9.9 — controls.  The same operations at the end of the last real
 * line were always correct and must stay correct: this change alters
 * what records mean, so the non-EOF paths need re-testing too. */
void test_control_roundtrip_newline_at_eol(void) {
	eofRoundTrip(OP_RET, 0);
}
void test_control_roundtrip_self_insert_at_eol(void) {
	eofRoundTrip(OP_SELF_INSERT, 0);
}
void test_control_roundtrip_yank_at_eol(void) {
	eofRoundTrip(OP_YANK, 0);
}

/* §1 — the original symptom.  Five repetitions of M-> RET C-_ used to
 * grow a 2-row buffer to 7 rows, one leaked blank line at a time. */
void test_eof_repeated_newline_undo_does_not_accumulate(void) {
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	char *original = contentOf(buf);

	for (int i = 0; i < 5; i++) {
		gotoVirtualEOF(buf);
		processKeypress(CMD_NEWLINE);
		processKeypress(CMD_UNDO);
	}

	char *after = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, after);
	TEST_ASSERT_EQUAL(3, buf->numrows);

	free(original);
	free(after);
	cleanupTestEditor();
}

/* §1 — redo over-inserted: RET at EOF, undo, redo gave two blank lines
 * where the original operation produced one. */
void test_eof_newline_redo_inserts_one_row_not_two(void) {
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);

	processKeypress(CMD_NEWLINE);
	TEST_ASSERT_EQUAL(4, buf->numrows);
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL(3, buf->numrows);
	processKeypress(CMD_REDO);
	TEST_ASSERT_EQUAL(4, buf->numrows);

	cleanupTestEditor();
}

/* §9.2 — multi-row payloads through the mutate path: line counts,
 * leading and trailing newlines, newlines only, and multi-byte text. */
static void eofYankRoundTrip(const char *payload, const char *expect) {
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);

	char *original = contentOf(buf);
	setKillText(payload);
	processKeypress(CMD_YANK);

	char *edited = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(expect, edited);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	redoAll(buf);
	char *redone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(edited, redone);

	free(original);
	free(edited);
	free(undone);
	free(redone);
	clearText(&E.kill);
	cleanupTestEditor();
}

/* Scrolling used to carry the cursor's byte offset onto a different row
 * and clamp it only against that row's byte length, so scrolling from a
 * long ASCII line onto a line of multibyte text left the cursor inside a
 * character.  The fuzzer reported this as "cursor is mid-character" and
 * it was the only failure category it had.
 *
 * Driven through processKeypress rather than by calling scrollLineUp
 * directly, because the rule is enforced by clampPositions at the end
 * of every command -- the invariant holds at command boundaries, not at
 * every instant inside one. */
void test_scroll_leaves_cursor_on_char_boundary(void) {
	initTestEditor();
	/* Row 0 is three 3-byte characters; row 1 is ASCII and longer,
	 * so a byte offset legal on row 1 falls inside a character on
	 * row 0. */
	static const char *lines[2] = {
		"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e",
		"abcdefghij"
	};
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	E.buf = buf;
	E.windows[0]->buf = buf;
	E.windows[0]->height = 1;
	E.windows[0]->rowoff = 1;

	buf->cy = 1;
	buf->cx = 7; /* inside row 0's third character */
	processKeypress(CMD_SCROLL_UP);

	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT(buf->cx == buf->row[0].size ||
		    !utf8_isCont(buf->row[0].chars[buf->cx]));
	cleanupTestEditor();
}

/* #105 acceptance (§10).  The mirror of the proof that motivated this
 * task.
 *
 * Before #105, a hand-built undo record naming the insertion's own
 * logical coordinates could not be replayed: an insertion at the end of
 * a two-row buffer produced the record (0,2)->(0,3), and bulkDelete
 * could not address row 3 because row 3 was not in the array.  The
 * guard in bulkDelete returned having deleted nothing, undo left the
 * row behind, and anchorInsert existed to translate such positions onto
 * a row that did exist.
 *
 * The defect was in the representation's inability to name the
 * position, not in the record.  With the trailing newline held as a
 * real row, logical and addressable coordinates are the same thing, so
 * the record that used to be unreplayable is now the only form there
 * is.  Same construction as the original proof, expectation inverted.
 *
 * Built by hand rather than through an editing command on purpose: this
 * asserts that the representation admits the record, independently of
 * whatever coordinates the mutation layer happens to produce. */
void test_logical_insert_record_round_trips(void) {
	initTestEditor();
	const char *lines[] = { "a", "b" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	E.buf = buf;
	clearUndosAndRedos(buf);

	char *before = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING("a\nb\n", before);
	int rows_before = buf->numrows;

	/* The record names where the edit happened: (0,2) -> (0,3). */
	struct undo *u = newUndo();
	u->startx = 0;
	u->starty = 2;
	u->endx = 0;
	u->endy = 3;
	u->data[0] = '\n';
	u->datalen = 1;
	u->delete = 0;
	pushUndo(buf, u);

	bulkInsert(buf, 0, 2, (const uint8_t *)"\n", 1);
	TEST_ASSERT_EQUAL_INT(rows_before + 1, buf->numrows);

	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_INT(rows_before, buf->numrows);
	char *after = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(before, after);

	doRedo(buf, 1);
	TEST_ASSERT_EQUAL_INT(rows_before + 1, buf->numrows);

	free(before);
	free(after);
	cleanupTestEditor();
}

/* RETIRED in #105 phase 1 -- mechanism tests for anchorInsert.
 *
 * Removed here: test_anchor_insert_rule,
 * test_virtual_eof_records_state_the_whole_change,
 * test_record_equality_newline_at_eol_vs_virtual_eof,
 * test_empty_buffer_insert_undo_is_lossy_by_design.
 *
 * They asserted anchorInsert's return coordinates and the raw
 * startx/starty/endx/endy of the records it built -- the existence and
 * exact output of a compensation #105 removes on purpose.  A test that
 * pins the mechanism cannot survive the change it is measuring.
 *
 * The behaviour they protected is retained, and asserted on serialised
 * content instead: an edit at the end of the buffer undoes back to the
 * original text, redo reinstates it, and no spurious row is left
 * behind.  See the eof_payload_* family below and the virtual-EOF
 * tests in test_coalesce.c.
 *
 * The last of the four pinned the rowless-restore limitation (undo of
 * an insert into an empty buffer left one row rather than none).
 * numrows >= 1 makes a rowless buffer unreachable, so there is no
 * longer a limitation to pin. */

void test_eof_payload_one_line(void) {
	eofYankRoundTrip("one", "alpha\nbeta\none\n");
}
void test_eof_payload_two_lines(void) {
	eofYankRoundTrip("one\ntwo", "alpha\nbeta\none\ntwo\n");
}
void test_eof_payload_three_lines(void) {
	eofYankRoundTrip("one\ntwo\nthree", "alpha\nbeta\none\ntwo\nthree\n");
}
void test_eof_payload_leading_newline(void) {
	eofYankRoundTrip("\none", "alpha\nbeta\n\none\n");
}
void test_eof_payload_newlines_only(void) {
	eofYankRoundTrip("\n\n", "alpha\nbeta\n\n\n");
}
void test_eof_payload_cjk(void) {
	eofYankRoundTrip("\xe6\x97\xa5\xe6\x9c\xac\n\xe8\xaa\x9e",
			 "alpha\nbeta\n\xe6\x97\xa5\xe6\x9c\xac\n\xe8\xaa\x9e\n");
}

/* §9.3 — a payload ending in "\n" at the virtual EOF: the divergence
 * that made bulkInsert and the typed paths disagree.  bulkInsert used
 * to add a row the typed path never would.  The row array supplies the
 * final terminator, so "one\n" and "one" land identically here — this
 * is the one bounded, user-visible behaviour change. */
void test_eof_payload_trailing_newline_adds_no_extra_row(void) {
	eofYankRoundTrip("one\n", "alpha\nbeta\none\n");

	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	setKillText("one\n");
	processKeypress(CMD_YANK);
	TEST_ASSERT_EQUAL(4, buf->numrows);
	clearText(&E.kill);
	cleanupTestEditor();
}


/* §9.5 — the regression guard for mutateReplace.  The anchor lands at
 * the end of the last row, which is exactly where a mark set with
 * C-SPC at end of buffer sits, and adjustPoint's insert branch treats a
 * point exactly at startx as being after the insertion.  So if the
 * point adjustment is fed the anchored range instead of the logical
 * one, this mark silently moves.  Nothing else in the plan catches it. */
void test_mark_unmoved_by_newline_at_virtual_eof(void) {
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	/* Mark at the end of the last row of text -- "beta", row 1.  That
	 * is the position the anchor used to map onto, and where
	 * adjustPoint's insert branch is most likely to move a point it
	 * should leave alone.  Written as an explicit row rather than
	 * numrows - 1, which since #105 names the trailing empty row and
	 * would put the mark somewhere else entirely. */
	buf->cy = 1;
	buf->cx = buf->row[1].size;
	processKeypress(CMD_SET_MARK);
	int markx = buf->markx, marky = buf->marky;
	TEST_ASSERT_EQUAL(4, markx);
	TEST_ASSERT_EQUAL(1, marky);

	gotoVirtualEOF(buf);
	processKeypress(CMD_NEWLINE);

	TEST_ASSERT_EQUAL(markx, buf->markx);
	TEST_ASSERT_EQUAL(marky, buf->marky);

	cleanupTestEditor();
}

/* Same guard on the mutate path, where the hazard actually lives:
 * bulkInsert adjusts points internally from whatever range it is
 * handed, so anchoring the mutation must not anchor the adjustment. */
void test_mark_unmoved_by_yank_at_virtual_eof(void) {
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	/* Mark at the end of the last row of text -- "beta", row 1.  That
	 * is the position the anchor used to map onto, and where
	 * adjustPoint's insert branch is most likely to move a point it
	 * should leave alone.  Written as an explicit row rather than
	 * numrows - 1, which since #105 names the trailing empty row and
	 * would put the mark somewhere else entirely. */
	buf->cy = 1;
	buf->cx = buf->row[1].size;
	processKeypress(CMD_SET_MARK);
	int markx = buf->markx, marky = buf->marky;

	/* End of buffer.  cy == numrows is unreachable under #105, so
	 * this is the last real row -- the same place in the text. */
	buf->cy = buf->numrows - 1;
	buf->cx = buf->row[buf->cy].size;
	setKillText("\n");
	processKeypress(CMD_YANK);

	TEST_ASSERT_EQUAL(markx, buf->markx);
	TEST_ASSERT_EQUAL(marky, buf->marky);

	clearText(&E.kill);
	cleanupTestEditor();
}

/* §9.6 — insertNewlineAndIndent calls undoAppendChar again for the
 * indent bytes after the newline has already moved the cursor, so the
 * compound path exercises the fresh-record and continuation branches in
 * one operation. */
void test_newline_and_indent_at_virtual_eof(void) {
	initTestEditor();
	static const char *lines[2] = { "alpha", "\t  beta" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	gotoVirtualEOF(buf);

	char *original = contentOf(buf);
	processKeypress(CMD_NEWLINE_INDENT);
	char *edited = contentOf(buf);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	redoAll(buf);
	char *redone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(edited, redone);

	free(original);
	free(edited);
	free(undone);
	free(redone);
	cleanupTestEditor();
}

/* §9.7 — repeat counts.  C-u 5 <char> takes undoSelfInsert's count > 1
 * branch, which builds a whole record in one go rather than appending. */
void test_repeat_count_self_insert_at_virtual_eof(void) {
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	char *original = contentOf(buf);

	E.uarg = 5;
	E.self_insert_key = 'q';
	processKeypress(CMD_SELF_INSERT);

	char *edited = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING("alpha\nbeta\nqqqqq\n", edited);
	free(edited);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	free(original);
	free(undone);
	cleanupTestEditor();
}

void test_repeat_count_newline_at_virtual_eof(void) {
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	char *original = contentOf(buf);

	E.uarg = 5;
	processKeypress(CMD_NEWLINE);
	TEST_ASSERT_EQUAL(8, buf->numrows);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);
	TEST_ASSERT_EQUAL(3, buf->numrows);

	free(original);
	free(undone);
	cleanupTestEditor();
}


/* §10 — chain_to_prev.  yankRectangle combines mutateExtendRows with a
 * chained mutateReplace; the two must not both account for the same
 * newline.  mutateExtendRows runs first and makes the target a real
 * row, so anchorInsert is the identity inside the replace. */
void test_rectangle_yank_at_virtual_eof_roundtrip(void) {
	initTestEditor();
	struct buffer *buf = eofTestBuffer();
	gotoVirtualEOF(buf);
	char *original = contentOf(buf);

	clearText(&E.kill);
	E.kill.str = (uint8_t *)xstrdup("abcd"); /* 2 wide, 2 high, flat */
	E.kill.is_rectangle = 1;
	E.kill.rect_width = 2;
	E.kill.rect_height = 2;
	processKeypress(CMD_YANK);

	char *edited = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING("alpha\nbeta\nab\ncd\n", edited);

	/* The replace's own insert record is unanchored: the extension
	 * already accounted for both newlines. */
	TEST_ASSERT_EQUAL(0, buf->undo->startx);
	TEST_ASSERT_EQUAL(2, buf->undo->starty);

	undoAll(buf);
	char *undone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(original, undone);

	redoAll(buf);
	char *redone = contentOf(buf);
	TEST_ASSERT_EQUAL_STRING(edited, redone);

	free(original);
	free(edited);
	free(undone);
	free(redone);
	clearText(&E.kill);
	cleanupTestEditor();
}




void setUp(void) {
}

void tearDown(void) {
}

/* Defect: the redo chain had no exit.  After C-/ left more to redo,
 * emil entered a mode where C-_ continued redoing -- but the mode was
 * never cleared when the redo stack emptied, so every later undo
 * keystroke was swallowed as a redo of nothing and undo appeared dead
 * until some unrelated key was pressed.  Found by fuzz_undo.c as
 * "kill-para (uarg 6), undo (uarg 5), redo" failing to restore. */
void test_redo_chain_releases_undo(void) {
	initTestEditor();
	const char *lines[] = { "alpha beta", "(gamma delta).", "  indented",
				"", "epsilon" };
	struct buffer *buf = make_test_buffer_lines(lines, 5);
	E.buf = buf;
	clearUndosAndRedos(buf);

	E.uarg = 6;
	processKeypress(CMD_KILL_PARA);
	E.uarg = 5;
	processKeypress(CMD_UNDO);
	E.uarg = 0;
	processKeypress(CMD_REDO);

	/* Undo must reach the original content rather than stalling. */
	for (int k = 0; k < 64 && buf->undo != NULL; k++) {
		E.uarg = 0;
		processKeypress(CMD_UNDO);
	}

	TEST_ASSERT_NULL(buf->undo);
	TEST_ASSERT_EQUAL_INT(6, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("alpha beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("epsilon", row_str(buf, 4));
	cleanupTestEditor();
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
	RUN_TEST(test_forward_search_does_not_wrap_on_first_pass);
	RUN_TEST(test_forward_search_repeat_wraps_after_failing);
	RUN_TEST(test_reverse_search_does_not_wrap_on_first_pass);
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

	/* #102 — undo records for insertions at the virtual EOF line */
	RUN_TEST(test_eof_roundtrip_newline);
	RUN_TEST(test_eof_roundtrip_open_line);
	RUN_TEST(test_eof_roundtrip_newline_indent);
	RUN_TEST(test_eof_roundtrip_self_insert);
	RUN_TEST(test_eof_roundtrip_tab);
	RUN_TEST(test_eof_roundtrip_yank);
	RUN_TEST(test_control_roundtrip_newline_at_eol);
	RUN_TEST(test_control_roundtrip_self_insert_at_eol);
	RUN_TEST(test_control_roundtrip_yank_at_eol);
	RUN_TEST(test_eof_repeated_newline_undo_does_not_accumulate);
	RUN_TEST(test_eof_newline_redo_inserts_one_row_not_two);
	RUN_TEST(test_scroll_leaves_cursor_on_char_boundary);
	RUN_TEST(test_logical_insert_record_round_trips);
	RUN_TEST(test_eof_payload_one_line);
	RUN_TEST(test_eof_payload_two_lines);
	RUN_TEST(test_eof_payload_three_lines);
	RUN_TEST(test_eof_payload_leading_newline);
	RUN_TEST(test_eof_payload_newlines_only);
	RUN_TEST(test_eof_payload_cjk);
	RUN_TEST(test_eof_payload_trailing_newline_adds_no_extra_row);
	RUN_TEST(test_mark_unmoved_by_newline_at_virtual_eof);
	RUN_TEST(test_mark_unmoved_by_yank_at_virtual_eof);
	RUN_TEST(test_newline_and_indent_at_virtual_eof);
	RUN_TEST(test_repeat_count_self_insert_at_virtual_eof);
	RUN_TEST(test_repeat_count_newline_at_virtual_eof);
	RUN_TEST(test_rectangle_yank_at_virtual_eof_roundtrip);

	RUN_TEST(test_redo_chain_releases_undo);

	return TEST_END();
}
