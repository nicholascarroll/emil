/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_focus.c: which buffer a keystroke edits.
 *
 * E.buf and the windows' focused flags are two records of one fact --
 * the buffer the user is typing into -- and the focus invariant in
 * emil.h says they agree between commands.  These cases cover the
 * places that broke it: popups that moved focus to window 0, the
 * palette restoring focus by buffer rather than by window, and
 * commands that moved E.buf off the minibuffer during a prompt. */

#include "test.h"
#include "test_harness.h"
#include "buffer.h"
#include "palette.h"
#include "prompt.h"
#include "register.h"
#include "window.h"
#include <string.h>
#include <stdlib.h>

/* A buffer holding `lines`, named `name`, linked at the head of the
 * buffer list but shown in no window. */
static struct buffer *addBuffer(const char *name, const char **lines, int n) {
	struct buffer *b = newBuffer();
	for (int i = 0; i < n; i++)
		insertRow(b, i, (const uint8_t *)lines[i], strlen(lines[i]));
	bufferEnsureRow(b);
	b->filename = xstrdup(name);
	b->next = E.headbuf;
	E.headbuf = b;
	return b;
}

/* Split screen, lower window focused: the layout in which "focus goes
 * to window 0" and "focus stays where it was" differ.  Window 0 shows
 * `upper`, window 1 shows `lower`, and E.buf is `lower`. */
static void splitLowerFocused(struct buffer *upper, struct buffer *lower) {
	E.windows = xrealloc(E.windows, 2 * sizeof(struct window *));
	E.windows[1] = xcalloc(1, sizeof(struct window));
	E.nwindows = 2;
	E.windows[0]->buf = upper;
	E.windows[1]->buf = lower;
	E.windows[0]->height = 10;
	E.windows[1]->height = 10;
	E.windows[0]->focused = 0;
	E.windows[1]->focused = 1;
	E.buf = lower;
}

static const char *one_line[] = { "text" };

/* ---- the check itself ---- */

void test_invariant_holds_for_a_consistent_split(void) {
	struct buffer *a = addBuffer("a.txt", one_line, 1);
	struct buffer *b = addBuffer("b.txt", one_line, 1);
	splitLowerFocused(a, b);
	TEST_ASSERT_NULL(focusInvariantBreach());
}

void test_invariant_reports_ebuf_in_an_unfocused_window(void) {
	struct buffer *a = addBuffer("a.txt", one_line, 1);
	struct buffer *b = addBuffer("b.txt", one_line, 1);
	splitLowerFocused(a, b);
	E.buf = a;
	TEST_ASSERT_NOT_NULL(focusInvariantBreach());
}

void test_invariant_reports_two_focused_windows(void) {
	struct buffer *a = addBuffer("a.txt", one_line, 1);
	struct buffer *b = addBuffer("b.txt", one_line, 1);
	splitLowerFocused(a, b);
	E.windows[0]->focused = 1;
	TEST_ASSERT_NOT_NULL(focusInvariantBreach());
}

void test_invariant_reports_no_focused_window(void) {
	struct buffer *a = addBuffer("a.txt", one_line, 1);
	struct buffer *b = addBuffer("b.txt", one_line, 1);
	splitLowerFocused(a, b);
	E.windows[1]->focused = 0;
	TEST_ASSERT_NOT_NULL(focusInvariantBreach());
}

/* ---- popups ---- */

/* showPopupBuffer() used to focus window 0 unconditionally. */
void test_popup_leaves_focus_where_it_was(void) {
	struct buffer *a = addBuffer("a.txt", one_line, 1);
	struct buffer *b = addBuffer("b.txt", one_line, 1);
	splitLowerFocused(a, b);

	struct buffer *pop = findOrCreateSpecialBuffer("*Test Popup*");
	bufferEnsureRow(pop);
	showPopupBuffer(pop);

	TEST_ASSERT_EQUAL_INT(3, E.nwindows);
	if (E.nwindows == 3) {
		TEST_ASSERT_FALSE(E.windows[0]->focused);
		TEST_ASSERT_TRUE(E.windows[1]->focused);
		TEST_ASSERT_FALSE(E.windows[2]->focused);
	}
	TEST_ASSERT_NULL(focusInvariantBreach());

	closeSpecialBuffer("*Test Popup*");
	TEST_ASSERT_EQUAL_INT(2, E.nwindows);
	TEST_ASSERT_TRUE(E.windows[1]->focused);
	TEST_ASSERT(E.buf == b);
	TEST_ASSERT_NULL(focusInvariantBreach());
}

/* Observes the prompt from inside: called after every key. */
static int popup_seen;
static int focus_moved_while_open;
static struct buffer *callback_target;
static void watchPopup(struct buffer *target, uint8_t *text, int key) {
	(void)text;
	(void)key;
	callback_target = target;
	if (findBufferByName("*Completions*") == NULL)
		return;
	popup_seen = 1;
	if (!E.windows[1]->focused || E.windows[0]->focused)
		focus_moved_while_open = 1;
}

/* The reported bug: C-g on a completion list, from the lower window,
 * left the cursor drawn in the upper window while E.buf -- and so the
 * next keystroke -- stayed with the lower one. */
void test_cancelled_completion_list_keeps_focus(void) {
	struct buffer *a = addBuffer("a.txt", one_line, 1);
	struct buffer *b = addBuffer("b.txt", one_line, 1);
	addBuffer("c1.txt", one_line, 1);
	addBuffer("c2.txt", one_line, 1);
	splitLowerFocused(a, b);

	popup_seen = 0;
	focus_moved_while_open = 0;
	callback_target = NULL;
	int keys[] = { 'c', '\t', '\t', CTRL('g') };
	scriptKeys(keys, 4);
	muteStdout();
	uint8_t *r = editorPrompt("Buffer: ", PROMPT_BUFFER, watchPopup);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_NULL(r);
	TEST_ASSERT_TRUE(popup_seen); /* or the case proves nothing */
	TEST_ASSERT_FALSE(focus_moved_while_open);
	/* The callback is handed the buffer the prompt was opened from,
	 * not the minibuffer that E.buf names meanwhile. */
	TEST_ASSERT(callback_target == b);
	TEST_ASSERT_EQUAL_INT(2, E.nwindows);
	TEST_ASSERT_TRUE(E.windows[1]->focused);
	TEST_ASSERT(E.buf == b);
	TEST_ASSERT_NULL(focusInvariantBreach());
	free(r);
}

/* The other half: accepting from the list switched the *upper* window,
 * because by then it was the focused one. */
void test_buffer_chosen_from_completion_list_opens_in_own_window(void) {
	struct buffer *a = addBuffer("a.txt", one_line, 1);
	struct buffer *b = addBuffer("b.txt", one_line, 1);
	struct buffer *c1 = addBuffer("c1.txt", one_line, 1);
	addBuffer("c2.txt", one_line, 1);
	splitLowerFocused(a, b);

	int keys[] = { 'c', '\t', '\t', '1', '.', 't', 'x', 't', '\r' };
	scriptKeys(keys, 9);
	muteStdout();
	switchToNamedBuffer();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT(E.windows[0]->buf == a);
	TEST_ASSERT(E.windows[1]->buf == c1);
	TEST_ASSERT(E.buf == c1);
	TEST_ASSERT_NULL(focusInvariantBreach());
}

/* C-x r v shows *Output* through the same popup path, outside any
 * prompt: the invariant broke at the top of the main loop. */
void test_view_register_keeps_focus(void) {
	struct buffer *a = addBuffer("a.txt", one_line, 1);
	struct buffer *b = addBuffer("b.txt", one_line, 1);
	splitLowerFocused(a, b);

	int keys[] = { 'a' };
	scriptKeys(keys, 1);
	muteStdout();
	viewRegister();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_NOT_NULL(findBufferByName("*Output*"));
	TEST_ASSERT_TRUE(E.windows[1]->focused);
	TEST_ASSERT(E.buf == b);
	TEST_ASSERT_NULL(focusInvariantBreach());
}

/* ---- palette ---- */

static const char *three_lines[] = { "line one", "line two", "line three" };

/* Both halves of the split show the same buffer.  Restoring focus "to
 * the window showing origin" found the upper one first. */
void test_palette_cancel_returns_to_own_window(void) {
	struct buffer *a = addBuffer("a.txt", three_lines, 3);
	splitLowerFocused(a, a);

	int keys[] = { CTRL('g') };
	scriptKeys(keys, 1);
	muteStdout();
	expandPalette();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_INT(2, E.nwindows);
	TEST_ASSERT_FALSE(E.windows[0]->focused);
	TEST_ASSERT_TRUE(E.windows[1]->focused);
	TEST_ASSERT(E.buf == a);
	TEST_ASSERT_NULL(focusInvariantBreach());
}

/* The symbol goes where the user was.  This pins the order inside
 * restoreFocusTo(): closing the palette while its window still has
 * focus makes destroyWindow() switch to window 0, which resets the
 * shared buffer's cursor to window 0's saved position -- here the
 * start of line one. */
void test_palette_insert_lands_at_own_cursor(void) {
	struct buffer *a = addBuffer("a.txt", three_lines, 3);
	splitLowerFocused(a, a);
	E.windows[0]->cx = 0; /* upper window's saved cursor */
	E.windows[0]->cy = 0;
	a->cy = 2; /* lower window: end of line three */
	a->cx = a->row[2].size;

	int keys[] = { '\r' };
	scriptKeys(keys, 1);
	muteStdout();
	expandPalette();
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_EQUAL_STRING("line one", row_str(a, 0));
	TEST_ASSERT_TRUE(strncmp(row_str(a, 2), "line three", 10) == 0);
	TEST_ASSERT_TRUE(a->row[2].size > 10); /* something was inserted */
	TEST_ASSERT_TRUE(E.windows[1]->focused);
	TEST_ASSERT_NULL(focusInvariantBreach());
}

/* ---- the minibuffer ---- */

/* Set by the prompt callback when a key was refused. */
static int refused_seen;
static void noteRefusal(struct buffer *target, uint8_t *text, int key) {
	(void)target;
	(void)text;
	(void)key;
	if (strcmp(E.statusmsg, "Not available in the minibuffer") == 0)
		refused_seen = 1;
}

/* Each command that must not run while a prompt reads the minibuffer,
 * as the keys that invoke it. */
static const struct {
	const char *name;
	int keys[3];
	int nkeys;
} refusals[] = {
	{ "other-window", { CTRL('x'), 'o' }, 2 },
	{ "split-window", { CTRL('x'), '2' }, 2 },
	{ "delete-window", { CTRL('x'), '0' }, 2 },
	{ "delete-other-windows", { CTRL('x'), '1' }, 2 },
	{ "next-buffer", { CTRL('x'), KEY_ARROW_RIGHT }, 2 },
	{ "previous-buffer", { CTRL('x'), KEY_ARROW_LEFT }, 2 },
	{ "kill-buffer", { CTRL('x'), 'k' }, 2 },
	{ "toggle-read-only", { CTRL('x'), CTRL('q') }, 2 },
	{ "jump-to-register", { CTRL('x'), 'r', 'j' }, 3 },
	{ "ctags-jump", { KEY_META('.') }, 1 },
	{ "ctags-back", { KEY_META(',') }, 1 },
	{ "toggle-header-body", { KEY_META('`') }, 1 },
};

/* Run the command inside a prompt, then type "q" and submit.  The
 * prompt must report the refusal, hand back "q" -- so the typing went
 * to the minibuffer -- and leave the editor as it found it.
 *
 * Without the refusals, next-buffer and kill-buffer crash;
 * previous-buffer and jump-to-register move E.buf to the other file,
 * so the "q" lands there; toggle-read-only leaves the minibuffer
 * read-only for every later prompt. */
void test_minibuffer_refuses_buffer_and_window_commands(void) {
	const char *file_text[] = { "file text" };
	struct buffer *other = addBuffer("other.txt", one_line, 1);
	struct buffer *file = addBuffer("file.txt", file_text, 1);
	E.buf = file;
	E.windows[0]->buf = file;

	/* Register a: a point in the other buffer, for jump-to-register. */
	E.registers['a'].rtype = REGISTER_POINT;
	E.registers['a'].data.point.buf = other;
	E.registers['a'].data.point.cx = 0;
	E.registers['a'].data.point.cy = 0;

	int n = (int)(sizeof(refusals) / sizeof(refusals[0]));
	for (int i = 0; i < n; i++) {
		int keys[8];
		int k = 0;
		for (int j = 0; j < refusals[i].nkeys; j++)
			keys[k++] = refusals[i].keys[j];
		keys[k++] = 'q';
		keys[k++] = '\r';
		scriptKeys(keys, k);

		refused_seen = 0;
		muteStdout();
		uint8_t *r = editorPrompt("Test: ", PROMPT_PLAIN, noteRefusal);
		unmuteStdout();
		clearKeys();

		int ok = refused_seen && r != NULL &&
			 strcmp((char *)r, "q") == 0 && E.buf == file &&
			 E.windows[0]->buf == file && E.nwindows == 1 &&
			 E.headbuf == file && file->next == other &&
			 strcmp(row_str(file, 0), "file text") == 0 &&
			 !E.minibuf->read_only &&
			 focusInvariantBreach() == NULL;
		if (!ok)
			printf("  refusal case: %s\n", refusals[i].name);
		TEST_ASSERT_TRUE(ok);
		free(r);
	}
}

/* The prompt loop resolved every key twice, so C-x arrived as C-x C-x
 * and no C-x command reached the minibuffer.  C-x h (mark the whole
 * buffer) then C-w must clear what was typed. */
void test_cx_chord_reaches_the_minibuffer(void) {
	addBuffer("file.txt", one_line, 1);
	E.buf = E.headbuf;
	E.windows[0]->buf = E.headbuf;

	int keys[] = { 'z', 'z', 'z', CTRL('x'), 'h', CTRL('w'), 'q', '\r' };
	scriptKeys(keys, 8);
	muteStdout();
	uint8_t *r = editorPrompt("Test: ", PROMPT_PLAIN, NULL);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_NOT_NULL(r);
	if (r)
		TEST_ASSERT_EQUAL_STRING("q", (char *)r);
	free(r);
}

/* A prompt key typed straight after C-x keeps its prompt meaning, and
 * the abandoned chord does not leak into the next command. */
void test_prompt_key_after_cx_abandons_the_chord(void) {
	addBuffer("file.txt", one_line, 1);
	E.buf = E.headbuf;
	E.windows[0]->buf = E.headbuf;

	int keys[] = { 'a', CTRL('x'), '\r' };
	scriptKeys(keys, 3);
	muteStdout();
	uint8_t *r = editorPrompt("Test: ", PROMPT_PLAIN, NULL);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT_NOT_NULL(r);
	if (r)
		TEST_ASSERT_EQUAL_STRING("a", (char *)r);
	free(r);
	TEST_ASSERT_EQUAL_INT(CMD_SELF_INSERT, resolveBinding('o'));
}

/* ---- fixture ---- */

void setUp(void) {
	initTestEditor();
	makeMinibuffer();
}

void tearDown(void) {
	clearKeys();
	freeMinibuffer();
	cleanupTestEditor();
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_invariant_holds_for_a_consistent_split);
	RUN_TEST(test_invariant_reports_ebuf_in_an_unfocused_window);
	RUN_TEST(test_invariant_reports_two_focused_windows);
	RUN_TEST(test_invariant_reports_no_focused_window);
	RUN_TEST(test_popup_leaves_focus_where_it_was);
	RUN_TEST(test_cancelled_completion_list_keeps_focus);
	RUN_TEST(test_buffer_chosen_from_completion_list_opens_in_own_window);
	RUN_TEST(test_view_register_keeps_focus);
	RUN_TEST(test_palette_cancel_returns_to_own_window);
	RUN_TEST(test_palette_insert_lands_at_own_cursor);
	RUN_TEST(test_minibuffer_refuses_buffer_and_window_commands);
	RUN_TEST(test_cx_chord_reaches_the_minibuffer);
	RUN_TEST(test_prompt_key_after_cx_abandons_the_chord);

	return TEST_END();
}
