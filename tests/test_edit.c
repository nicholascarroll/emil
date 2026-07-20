/* test_edit.c — Parameterized edit primitives. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include "keymap.h"
#include <stdint.h>

/* ---- Character insertion ---- */

void test_insert_char_beginning(void) {
	struct buffer *buf = make_test_buffer("BCD");
	buf->cx = 0;
	insertChar(buf, 'A', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
}

void test_insert_char_middle(void) {
	struct buffer *buf = make_test_buffer("ACD");
	buf->cx = 1;
	insertChar(buf, 'B', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
}

void test_insert_char_end(void) {
	struct buffer *buf = make_test_buffer("ABC");
	buf->cx = 3;
	insertChar(buf, 'D', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
}

void test_insert_char_with_count(void) {
	struct buffer *buf = make_test_buffer("AE");
	buf->cx = 1;
	insertChar(buf, 'B', 3);
	TEST_ASSERT_EQUAL_STRING("ABBBE", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(4, buf->cx);
}

void test_insert_char_readonly(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->read_only = 1;
	buf->cx = 0;
	insertChar(buf, 'X', 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
}

/* Regression: a self-insert refused by a read-only buffer must not
 * record undo.  Previously processKeypress called undoSelfInsert
 * before insertChar's read-only check, so toggling the buffer
 * writable and undoing deleted text that was never inserted. */
void test_self_insert_readonly_records_no_undo(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->read_only = 1;
	buf->cx = 0;
	buf->cy = 0;

	/* Drive the real key path: CMD_SELF_INSERT of 'X'. */
	E.self_insert_key = 'X';
	processKeypress(CMD_SELF_INSERT);

	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_NULL(buf->undo);

	/* Make writable and undo: buffer must be unchanged. */
	buf->read_only = 0;
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
}

/* Regression: same for the multibyte path.  insertUnicode had no
 * read-only guard, so undoAppendUnicode recorded a phantom insert
 * whose undo removed a byte range that split UTF-8 sequences. */
void test_insert_unicode_readonly_records_no_undo(void) {
	struct buffer *buf = make_test_buffer("h\xC3\xA9llo"); /* héllo */
	buf->read_only = 1;
	buf->cx = 0;
	buf->cy = 0;

	E.unicode[0] = 0xC3; /* é */
	E.unicode[1] = 0xA9;
	E.nunicode = 2;
	insertUnicode(1);

	TEST_ASSERT_EQUAL_STRING("h\xC3\xA9llo", row_str(buf, 0));
	TEST_ASSERT_NULL(buf->undo);

	buf->read_only = 0;
	doUndo(buf, 1);
	TEST_ASSERT_EQUAL_STRING("h\xC3\xA9llo", row_str(buf, 0));
}

/* Regression: a refused self-insert must not shift tracked points.
 * Previously undoAppendChar ran adjustAllPoints for the phantom
 * insertion, drifting the mark in a read-only buffer. */
void test_self_insert_readonly_does_not_move_mark(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->read_only = 1;
	buf->cx = 0;
	buf->cy = 0;
	buf->markx = 3;
	buf->marky = 0;

	E.self_insert_key = 'X';
	processKeypress(CMD_SELF_INSERT);

	TEST_ASSERT_EQUAL(3, buf->markx);
	TEST_ASSERT_EQUAL(0, buf->marky);
}

/* ---- Newlines ---- */

void test_insert_newline_splits(void) {
	struct buffer *buf = make_test_buffer("HelloWorld");
	buf->cx = 5;
	E.buf = buf;
	insertNewline(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("World", row_str(buf, 1));
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
	TEST_ASSERT_EQUAL_INT(1, buf->cy);
}

void test_insert_newline_at_beginning(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 0;
	E.buf = buf;
	insertNewline(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 1));
}

void test_insert_newline_at_end(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 5;
	E.buf = buf;
	insertNewline(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 1));
}

void test_insert_newline_and_indent(void) {
	struct buffer *buf = make_test_buffer("    Hello");
	buf->cx = 9;
	E.buf = buf;
	insertNewlineAndIndent(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("    Hello", row_str(buf, 0));
	TEST_ASSERT(buf->row[1].size >= 4);
	TEST_ASSERT(buf->row[1].chars[0] == ' ');
	TEST_ASSERT(buf->row[1].chars[3] == ' ');
}

/* Regression: C-j at line 0 of a read-only buffer read row[-1]
 * (ASan crash); with the guard the buffer is simply untouched. */
void test_newline_indent_readonly_line0(void) {
	struct buffer *buf = make_test_buffer("\tHello");
	buf->read_only = 1;
	buf->cx = 0;
	E.buf = buf;
	insertNewlineAndIndent(1);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("\tHello", row_str(buf, 0));
}

void test_open_line(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 5;
	E.buf = buf;
	openLine(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT_EQUAL_INT(5, buf->cx);
}

/* ---- Deletion ---- */

void test_del_char_middle(void) {
	struct buffer *buf = make_test_buffer("ABCD");
	buf->cx = 1;
	E.buf = buf;
	delChar(1);
	TEST_ASSERT_EQUAL_STRING("ACD", row_str(buf, 0));
}

void test_del_char_joins_lines(void) {
	const char *lines[] = { "Hello", "World" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 5;
	buf->cy = 0;
	E.buf = buf;
	delChar(1);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("HelloWorld", row_str(buf, 0));
}

void test_backspace_middle(void) {
	struct buffer *buf = make_test_buffer("ABCD");
	buf->cx = 2;
	E.buf = buf;
	backSpace(1);
	TEST_ASSERT_EQUAL_STRING("ACD", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
}

void test_backspace_joins_lines(void) {
	const char *lines[] = { "Hello", "World" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 0;
	buf->cy = 1;
	E.buf = buf;
	backSpace(1);
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("HelloWorld", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(5, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
}

/* ---- Indentation ---- */

void test_unindent(void) {
	struct buffer *buf = make_test_buffer("\tHello");
	buf->cx = 1;
	buf->indent = 0;
	E.buf = buf;
	unindent(1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
}

void test_unindent_readonly(void) {
	struct buffer *buf = make_test_buffer("\tHello");
	buf->read_only = 1;
	buf->cx = 1;
	buf->indent = 0;
	E.buf = buf;
	unindent(1);
	TEST_ASSERT_EQUAL_STRING("\tHello", row_str(buf, 0));
}

/* ---- Paragraph boundary helpers ---- */

void test_forward_para_boundary(void) {
	const char *lines[] = { "Hello world.", "", "Second para." };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->cx = 0;
	buf->cy = 0;
	int cx = 0, cy = 0;
	E.buf = buf;
	forwardParaBoundary(&cx, &cy);
	TEST_ASSERT_EQUAL_INT(1, cy);
	TEST_ASSERT_EQUAL_INT(0, cx);
}

void test_backward_para_boundary(void) {
	const char *lines[] = { "Hello world.", "", "Second para." };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->cx = 0;
	buf->cy = 2;
	int cx = 0, cy = 2;
	E.buf = buf;
	backwardParaBoundary(&cx, &cy);
	TEST_ASSERT_EQUAL_INT(1, cy);
	TEST_ASSERT_EQUAL_INT(0, cx);
}

void test_forward_para_to_end(void) {
	const char *lines[] = { "One line only" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	buf->cx = 0;
	buf->cy = 0;
	int cx = 0, cy = 0;
	E.buf = buf;
	forwardParaBoundary(&cx, &cy);
	TEST_ASSERT_EQUAL_INT(1, cy); /* numrows */
}

void test_backward_para_to_start(void) {
	const char *lines[] = { "One line only" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	buf->cx = 5;
	buf->cy = 0;
	int cx = 5, cy = 0;
	E.buf = buf;
	backwardParaBoundary(&cx, &cy);
	TEST_ASSERT_EQUAL_INT(0, cy);
	TEST_ASSERT_EQUAL_INT(0, cx);
}

/* ---- Sentence movement ---- */

void test_forward_sentence_simple(void) {
	struct buffer *buf = make_test_buffer("Hello world. Goodbye world.");
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	int cx = 0, cy = 0;
	forwardSentenceEnd(&cx, &cy);
	/* Land on the space after the period of first sentence */
	TEST_ASSERT_EQUAL_INT(12, cx);  // "Hello world." → cursor after '.'
	TEST_ASSERT_EQUAL_INT(0, cy);
}

void test_forward_sentence_end_of_line(void) {
	const char *lines[] = { "Hello world.", "Next sentence." };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	int cx = 0, cy = 0;
	forwardSentenceEnd(&cx, &cy);
	/* Sentence ends at end-of-line — land at row->size */
	TEST_ASSERT_EQUAL_INT(12, cx);
	TEST_ASSERT_EQUAL_INT(0, cy);
}

void test_backward_sentence_simple(void) {
	struct buffer *buf = make_test_buffer("Hello world. Goodbye world.");
	buf->cx = 27; /* end of line */
	buf->cy = 0;
	E.buf = buf;
	int cx = 27, cy = 0;
	backwardSentenceStart(&cx, &cy);
	/* Land at start of "Goodbye" (cursor before 'G') */
	TEST_ASSERT_EQUAL_INT(13, cx);
	TEST_ASSERT_EQUAL_INT(0, cy);
}

void test_backward_sentence_to_beginning(void) {
	struct buffer *buf = make_test_buffer("Hello world.");
	buf->cx = 5;
	buf->cy = 0;
	E.buf = buf;
	int cx = 5, cy = 0;
	backwardSentenceStart(&cx, &cy);
	/* Land at start of buffer */
	TEST_ASSERT_EQUAL_INT(0, cx);
	TEST_ASSERT_EQUAL_INT(0, cy);
}


void test_forward_sentence_with_closing_punct(void) {
	struct buffer *buf = make_test_buffer("He said \"hello.\" Then left.");
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	int cx = 0, cy = 0;
	forwardSentenceEnd(&cx, &cy);
	/* No [Punct][Space][Upper] pattern matches — lands at end of line */
	TEST_ASSERT_EQUAL_INT(27, cx);
	TEST_ASSERT_EQUAL_INT(0, cy);
}

void test_forward_sentence_para_boundary(void) {
	const char *lines[] = { "First sentence", "", "Second sentence" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	int cx = 0, cy = 0;
	forwardSentenceEnd(&cx, &cy);
	/* No sentence-end pattern — lands at end of first line */
	TEST_ASSERT_EQUAL_INT(14, cx);
	TEST_ASSERT_EQUAL_INT(0, cy);
}

/* ---- Kill sexp ---- */

void test_kill_sexp_parens(void) {
	struct buffer *buf = make_test_buffer("(hello) world");
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	killSexp(1);
	TEST_ASSERT_EQUAL_STRING(" world", row_str(buf, 0));
}

void test_kill_sexp_word(void) {
	struct buffer *buf = make_test_buffer("hello world");
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	killSexp(1);
	TEST_ASSERT_EQUAL_STRING(" world", row_str(buf, 0));
}

void test_kill_sexp_readonly(void) {
	struct buffer *buf = make_test_buffer("(hello) world");
	buf->read_only = 1;
	buf->cx = 0;
	E.buf = buf;
	killSexp(1);
	TEST_ASSERT_EQUAL_STRING("(hello) world", row_str(buf, 0));
}

/* ---- Kill paragraph ---- */

void test_kill_paragraph(void) {
	const char *lines[] = { "Hello world.", "More text.", "",
				"Next para." };
	struct buffer *buf = make_test_buffer_lines(lines, 4);
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	killParagraph(1);
	/* Should kill up to the blank line */
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	/* First remaining line should be the blank line */
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 0));
}

void test_kill_paragraph_readonly(void) {
	const char *lines[] = { "Hello.", "", "World." };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->read_only = 1;
	E.buf = buf;
	killParagraph(1);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
}

/* ---- Mark paragraph ---- */

void test_mark_paragraph(void) {
	const char *lines[] = { "First.", "", "Hello.", "World.", "", "Last." };
	struct buffer *buf = make_test_buffer_lines(lines, 6);
	buf->cx = 3;
	buf->cy = 2;
	E.buf = buf;
	markParagraph();
	/* Point should be at paragraph start (blank line before) */
	TEST_ASSERT_EQUAL_INT(1, buf->cy);
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
	/* Mark should be at paragraph end (blank line after) */
	TEST_ASSERT_EQUAL_INT(4, buf->marky);
	TEST_ASSERT_EQUAL_INT(0, buf->markx);
	TEST_ASSERT_EQUAL_INT(1, buf->mark_active);
}

/* ---- Zap to char ---- */
/* Note: zapToChar reads a key interactively, so we can't easily
 * unit test it without mocking readKey. We test the boundary
 * cases that don't require key input through the delete range. */

/* ---- Paragraph movement via refactored functions ---- */

void test_editor_forward_para(void) {
	const char *lines[] = { "A", "B", "", "C", "D" };
	struct buffer *buf = make_test_buffer_lines(lines, 5);
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	forwardPara(1);
	TEST_ASSERT_EQUAL_INT(2, E.buf->cy);
}

void test_editor_back_para(void) {
	const char *lines[] = { "A", "B", "", "C", "D" };
	struct buffer *buf = make_test_buffer_lines(lines, 5);
	buf->cx = 0;
	buf->cy = 4;
	E.buf = buf;
	backPara(1);
	TEST_ASSERT_EQUAL_INT(2, E.buf->cy);
}

/* ---- Edge case tests ---- */

void test_insert_newline_empty_buffer(void) {
	struct buffer *buf = make_test_buffer(NULL);
	/* Start with one empty row, like a real empty file */
	insertRow(buf, 0, (const uint8_t *)"", 0);
	clearUndosAndRedos(buf);
	E.buf = buf;
	buf->cx = 0;
	buf->cy = 0;
	insertNewline(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 1));
}

void test_backspace_at_origin(void) {
	struct buffer *buf = make_test_buffer("Hello");
	E.buf = buf;
	buf->cx = 0;
	buf->cy = 0;
	backSpace(1);
	/* Should be a no-op */
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
}

void test_backspace_stray_continuation_byte(void) {
	/* Regression: a row beginning with a stray UTF-8 continuation
	 * byte (reachable via byte-column rectangle ops through
	 * multi-byte text) must not walk cx below 0.  Previously the
	 * continuation walk in backSpace had no lower bound and read
	 * chars[-1]. */
	struct buffer *buf = make_test_buffer("x");
	E.buf = buf;
	/* Replace row content with a lone continuation byte. */
	buf->row[0].chars[0] = 0x80;
	buf->cx = 1;
	buf->cy = 0;
	backSpace(1);
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->row[0].size);
}

void test_del_char_at_end_of_last_line(void) {
	struct buffer *buf = make_test_buffer("Hello");
	E.buf = buf;
	buf->cx = 5;
	buf->cy = 0;
	delChar(1);
	/* Should be a no-op — nothing after the last char of the last line */
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
}

void test_move_cursor_empty_buffer(void) {
	struct buffer *buf = make_test_buffer("");
	E.buf = buf;
	buf->cx = 0;
	buf->cy = 0;
	moveCursor(KEY_ARROW_UP, 1);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	moveCursor(KEY_ARROW_DOWN, 1);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	moveCursor(KEY_ARROW_LEFT, 1);
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
	moveCursor(KEY_ARROW_RIGHT, 1);
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
}

/* Regression: moving up from a long line onto a shorter one used to
 * read row->chars past the allocation when cx exceeded the shorter
 * row's size (heap-buffer-overflow under ASAN). */
void test_move_cursor_up_to_shorter_line(void) {
	const char *lines[] = { "ab", "this is a long line" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cy = 1;
	buf->cx = 15;
	moveCursor(KEY_ARROW_UP, 1);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT_EQUAL_INT(2, buf->cx); /* clamped to row length */
}

/* Regression: killLine and transposeChars with the cursor on the
 * virtual line past EOF (cy == numrows) used to index row[numrows],
 * reading past the row array (heap-buffer-overflow under ASAN when
 * numrows == rowcap). */
void test_kill_line_on_virtual_eof_line(void) {
	struct buffer *buf = newBuffer();
	E.buf = buf;
	E.headbuf = buf;
	E.windows[0]->buf = buf;
	/* Fill rowcap exactly (16) so row[numrows] is out of bounds */
	for (int i = 0; i < 16; i++)
		insertRow(buf, i, (const uint8_t *)"line", 4);
	buf->dirty = 0;
	clearUndosAndRedos(buf);
	buf->cy = buf->numrows;
	buf->cx = 0;
	killLine(1);
	TEST_ASSERT_EQUAL_INT(16, buf->numrows); /* no-op */
	transposeChars(0);
	TEST_ASSERT_EQUAL_INT(16, buf->numrows); /* no-op */
}

/* ---- M-- reverse modifier: transpose and word case ---- */

/* Forward baseline: C-t at "ab|cd" drags b forward → "acb|d". */
void test_transpose_chars_forward(void) {
	struct buffer *buf = make_test_buffer("abcd");
	buf->cx = 2;
	transposeChars(0);
	TEST_ASSERT_EQUAL_STRING("acbd", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(3, buf->cx);
}

/* M-- C-t at "abc|d" drags c backward past b → "ac|bd". */
void test_transpose_chars_reverse(void) {
	struct buffer *buf = make_test_buffer("abcd");
	buf->cx = 3;
	transposeChars(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("acbd", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(2, buf->cx);
}

/* Reverse then forward transpose at the same spot round-trips. */
void test_transpose_chars_reverse_roundtrip(void) {
	struct buffer *buf = make_test_buffer("abcd");
	buf->cx = 3;
	transposeChars(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("acbd", row_str(buf, 0));
	transposeChars(0);
	TEST_ASSERT_EQUAL_STRING("abcd", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(3, buf->cx);
}

/* M-- C-t with fewer than two characters before point: refused. */
void test_transpose_chars_reverse_needs_two_before(void) {
	struct buffer *buf = make_test_buffer("abcd");
	buf->cx = 1;
	transposeChars(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("abcd", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);

	buf->cx = 0;
	transposeChars(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("abcd", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
}

/* M-- C-t with multi-byte characters: "aé漢|" drags 漢 back past é.
 * "é" is 2 bytes (0xC3 0xA9), "漢" is 3 bytes (0xE6 0xBC 0xA2). */
void test_transpose_chars_reverse_utf8(void) {
	struct buffer *buf = make_test_buffer("a\xc3\xa9\xe6\xbc\xa2");
	buf->cx = 6; /* end of line, after 漢 */
	transposeChars(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("a\xe6\xbc\xa2\xc3\xa9", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(4, buf->cx); /* after the dragged 漢 */
}

/* M-- M-t at "alpha beta|" drags beta back past alpha → "beta| alpha". */
void test_transpose_words_reverse(void) {
	struct buffer *buf = make_test_buffer("alpha beta gamma");
	buf->cx = 10; /* end of "beta" */
	transposeWords(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("beta alpha gamma", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(4, buf->cx); /* after the dragged "beta" */
}

/* M-- M-t with point in trailing whitespace still acts on the two
 * words before point. */
void test_transpose_words_reverse_from_gap(void) {
	struct buffer *buf = make_test_buffer("one two three");
	buf->cx = 7; /* on the space after "two" */
	transposeWords(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("two one three", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(3, buf->cx); /* after the dragged "two" */
}

/* M-- M-t with only one word before point: refused, point restored. */
void test_transpose_words_reverse_needs_two_words(void) {
	struct buffer *buf = make_test_buffer("alpha beta");
	buf->cx = 5; /* end of "alpha", first word of the buffer */
	transposeWords(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("alpha beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(5, buf->cx);
}

/* M-- C-x C-t at "One. Two.|" drags the second sentence back past the
 * first, point after the dragged sentence. */
void test_transpose_sentences_reverse(void) {
	struct buffer *buf = make_test_buffer("Alpha one. Beta two.");
	buf->cx = 20; /* end of line, after the second sentence */
	transposeSentences(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING(" Beta two.Alpha one.", row_str(buf, 0));
}

/* Forward baseline for the sentence layout above, for symmetry: the
 * gap travels with the second segment in both directions. */
void test_transpose_sentences_forward_baseline(void) {
	struct buffer *buf = make_test_buffer("Alpha one. Beta two.");
	buf->cx = 11; /* start of "Beta" */
	transposeSentences(0);
	TEST_ASSERT_EQUAL_STRING(" Beta two.Alpha one.", row_str(buf, 0));
}

/* M-- M-u upcases the word before point without moving point. */
void test_upcase_word_reverse(void) {
	struct buffer *buf = make_test_buffer("alpha beta gamma");
	buf->cx = 10; /* end of "beta" */
	upcaseWord(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("alpha BETA gamma", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(10, buf->cx);
}

/* Forward M-u still moves point past the word it changes. */
void test_upcase_word_forward_moves_point(void) {
	struct buffer *buf = make_test_buffer("alpha beta");
	buf->cx = 0;
	upcaseWord(0);
	TEST_ASSERT_EQUAL_STRING("ALPHA beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(5, buf->cx);
}

/* M-- M-l and M-- M-c behave the same way on the previous word. */
void test_downcase_capitalize_word_reverse(void) {
	struct buffer *buf = make_test_buffer("ALPHA BETA");
	buf->cx = 10;
	downcaseWord(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("ALPHA beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(10, buf->cx);

	capitalCaseWord(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("ALPHA Beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(10, buf->cx);
}

/* M-- M-u at the start of the buffer: no word before point, no-op. */
void test_upcase_word_reverse_at_buffer_start(void) {
	struct buffer *buf = make_test_buffer("alpha");
	buf->cx = 0;
	upcaseWord(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("alpha", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(0, buf->cx);
}

/* Reverse word transforms respect read-only buffers: no edit, and
 * crucially no point movement from the repositioning step. */
void test_transpose_reverse_readonly(void) {
	struct buffer *buf = make_test_buffer("alpha beta");
	buf->cx = 10;
	buf->read_only = 1;
	transposeWords(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("alpha beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(10, buf->cx);
	transposeChars(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("alpha beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(10, buf->cx);
	upcaseWord(UARG_REVERSE);
	TEST_ASSERT_EQUAL_STRING("alpha beta", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(10, buf->cx);
	buf->read_only = 0;
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_insert_char_beginning);
	RUN_TEST(test_insert_char_middle);
	RUN_TEST(test_insert_char_end);
	RUN_TEST(test_insert_char_with_count);
	RUN_TEST(test_insert_char_readonly);
	RUN_TEST(test_self_insert_readonly_records_no_undo);
	RUN_TEST(test_insert_unicode_readonly_records_no_undo);
	RUN_TEST(test_self_insert_readonly_does_not_move_mark);

	RUN_TEST(test_insert_newline_splits);
	RUN_TEST(test_insert_newline_at_beginning);
	RUN_TEST(test_insert_newline_at_end);
	RUN_TEST(test_insert_newline_and_indent);
	RUN_TEST(test_newline_indent_readonly_line0);
	RUN_TEST(test_open_line);

	RUN_TEST(test_del_char_middle);
	RUN_TEST(test_del_char_joins_lines);
	RUN_TEST(test_backspace_middle);
	RUN_TEST(test_backspace_joins_lines);

	RUN_TEST(test_unindent);
	RUN_TEST(test_unindent_readonly);

	/* Paragraph boundary helpers */
	RUN_TEST(test_forward_para_boundary);
	RUN_TEST(test_backward_para_boundary);
	RUN_TEST(test_forward_para_to_end);
	RUN_TEST(test_backward_para_to_start);

	/* Sentence movement */
	RUN_TEST(test_forward_sentence_simple);
	RUN_TEST(test_forward_sentence_end_of_line);
	RUN_TEST(test_backward_sentence_simple);
	RUN_TEST(test_backward_sentence_to_beginning);
	RUN_TEST(test_forward_sentence_with_closing_punct);
	RUN_TEST(test_forward_sentence_para_boundary);

	/* Kill sexp */
	RUN_TEST(test_kill_sexp_parens);
	RUN_TEST(test_kill_sexp_word);
	RUN_TEST(test_kill_sexp_readonly);

	/* Kill paragraph */
	RUN_TEST(test_kill_paragraph);
	RUN_TEST(test_kill_paragraph_readonly);

	/* Mark paragraph */
	RUN_TEST(test_mark_paragraph);

	/* Paragraph movement (refactored) */
	RUN_TEST(test_editor_forward_para);
	RUN_TEST(test_editor_back_para);

	/* Edge cases */
	RUN_TEST(test_insert_newline_empty_buffer);
	RUN_TEST(test_backspace_at_origin);
	RUN_TEST(test_backspace_stray_continuation_byte);
	RUN_TEST(test_del_char_at_end_of_last_line);
	RUN_TEST(test_move_cursor_empty_buffer);
	RUN_TEST(test_move_cursor_up_to_shorter_line);
	RUN_TEST(test_kill_line_on_virtual_eof_line);

	/* M-- reverse modifier: transpose and word case */
	RUN_TEST(test_transpose_chars_forward);
	RUN_TEST(test_transpose_chars_reverse);
	RUN_TEST(test_transpose_chars_reverse_roundtrip);
	RUN_TEST(test_transpose_chars_reverse_needs_two_before);
	RUN_TEST(test_transpose_chars_reverse_utf8);
	RUN_TEST(test_transpose_words_reverse);
	RUN_TEST(test_transpose_words_reverse_from_gap);
	RUN_TEST(test_transpose_words_reverse_needs_two_words);
	RUN_TEST(test_transpose_sentences_reverse);
	RUN_TEST(test_transpose_sentences_forward_baseline);
	RUN_TEST(test_upcase_word_reverse);
	RUN_TEST(test_upcase_word_forward_moves_point);
	RUN_TEST(test_downcase_capitalize_word_reverse);
	RUN_TEST(test_upcase_word_reverse_at_buffer_start);
	RUN_TEST(test_transpose_reverse_readonly);

	return TEST_END();
}
