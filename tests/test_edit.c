/* test_edit.c — Parameterized edit primitives. */

#include "test.h"
#include "test_harness.h"
#include "edit.h"
#include <stdint.h>

/* ---- Character insertion ---- */

void test_insert_char_beginning(void) {
	struct buffer *buf = make_test_buffer("BCD");
	buf->cx = 0;
	insertChar(buf, 'A', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	destroyBuffer(buf);
}

void test_insert_char_middle(void) {
	struct buffer *buf = make_test_buffer("ACD");
	buf->cx = 1;
	insertChar(buf, 'B', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_insert_char_end(void) {
	struct buffer *buf = make_test_buffer("ABC");
	buf->cx = 3;
	insertChar(buf, 'D', 1);
	TEST_ASSERT_EQUAL_STRING("ABCD", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_insert_char_with_count(void) {
	struct buffer *buf = make_test_buffer("AE");
	buf->cx = 1;
	insertChar(buf, 'B', 3);
	TEST_ASSERT_EQUAL_STRING("ABBBE", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(4, buf->cx);
	destroyBuffer(buf);
}

void test_insert_char_readonly(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->read_only = 1;
	buf->cx = 0;
	insertChar(buf, 'X', 1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	destroyBuffer(buf);
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
	destroyBuffer(buf);
}

void test_insert_newline_at_beginning(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 0;
	E.buf = buf;
	insertNewline(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 1));
	destroyBuffer(buf);
}

void test_insert_newline_at_end(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 5;
	E.buf = buf;
	insertNewline(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	TEST_ASSERT_EQUAL_STRING("", row_str(buf, 1));
	destroyBuffer(buf);
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
	destroyBuffer(buf);
}

void test_open_line(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 5;
	E.buf = buf;
	openLine(1);
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT_EQUAL_INT(5, buf->cx);
	destroyBuffer(buf);
}

/* ---- Deletion ---- */

void test_del_char_middle(void) {
	struct buffer *buf = make_test_buffer("ABCD");
	buf->cx = 1;
	E.buf = buf;
	delChar(1);
	TEST_ASSERT_EQUAL_STRING("ACD", row_str(buf, 0));
	destroyBuffer(buf);
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
	destroyBuffer(buf);
}

void test_backspace_middle(void) {
	struct buffer *buf = make_test_buffer("ABCD");
	buf->cx = 2;
	E.buf = buf;
	backSpace(1);
	TEST_ASSERT_EQUAL_STRING("ACD", row_str(buf, 0));
	TEST_ASSERT_EQUAL_INT(1, buf->cx);
	destroyBuffer(buf);
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
	destroyBuffer(buf);
}

/* ---- Indentation ---- */

void test_indent_tab(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 0;
	buf->indent = 0;
	E.buf = buf;
	editorIndent(1);
	TEST_ASSERT_EQUAL_STRING("\tHello", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_indent_spaces(void) {
	struct buffer *buf = make_test_buffer("Hello");
	buf->cx = 0;
	buf->indent = 4;
	E.buf = buf;
	editorIndent(1);
	TEST_ASSERT_EQUAL_STRING("    Hello", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_unindent(void) {
	struct buffer *buf = make_test_buffer("\tHello");
	buf->cx = 1;
	buf->indent = 0;
	E.buf = buf;
	unindent(1);
	TEST_ASSERT_EQUAL_STRING("Hello", row_str(buf, 0));
	destroyBuffer(buf);
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
	destroyBuffer(buf);
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
	destroyBuffer(buf);
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
	destroyBuffer(buf);
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
	destroyBuffer(buf);
}

/* ---- Sentence movement ---- */

void test_forward_sentence_simple(void) {
	struct buffer *buf =
		make_test_buffer("Hello world. Goodbye world.");
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	int cx = 0, cy = 0;
	forwardSentenceEnd(&cx, &cy);
	/* Should land after "Hello world. " (pos 13) */
	TEST_ASSERT_EQUAL_INT(13, cx);
	TEST_ASSERT_EQUAL_INT(0, cy);
	destroyBuffer(buf);
}

void test_forward_sentence_end_of_line(void) {
	const char *lines[] = { "Hello world.", "Next sentence." };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	int cx = 0, cy = 0;
	forwardSentenceEnd(&cx, &cy);
	/* Sentence ends at ".", followed by end-of-line, land at start
	 * of next line */
	TEST_ASSERT_EQUAL_INT(0, cx);
	TEST_ASSERT_EQUAL_INT(1, cy);
	destroyBuffer(buf);
}

void test_backward_sentence_simple(void) {
	struct buffer *buf =
		make_test_buffer("Hello world. Goodbye world.");
	buf->cx = 27; /* end of line */
	buf->cy = 0;
	E.buf = buf;
	int cx = 27, cy = 0;
	backwardSentenceStart(&cx, &cy);
	/* Should land at start of "Goodbye" (pos 13) */
	TEST_ASSERT_EQUAL_INT(13, cx);
	TEST_ASSERT_EQUAL_INT(0, cy);
	destroyBuffer(buf);
}

void test_backward_sentence_to_beginning(void) {
	struct buffer *buf = make_test_buffer("Hello world.");
	buf->cx = 5;
	buf->cy = 0;
	E.buf = buf;
	int cx = 5, cy = 0;
	backwardSentenceStart(&cx, &cy);
	TEST_ASSERT_EQUAL_INT(0, cx);
	TEST_ASSERT_EQUAL_INT(0, cy);
	destroyBuffer(buf);
}

void test_forward_sentence_with_closing_punct(void) {
	struct buffer *buf =
		make_test_buffer("He said \"hello.\" Then left.");
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	int cx = 0, cy = 0;
	forwardSentenceEnd(&cx, &cy);
	/* Should skip past the closing quote: "hello.\" " — land at 'T' */
	TEST_ASSERT_EQUAL_INT(17, cx);
	TEST_ASSERT_EQUAL_INT(0, cy);
	destroyBuffer(buf);
}

void test_forward_sentence_para_boundary(void) {
	const char *lines[] = { "First sentence", "", "Second sentence" };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	int cx = 0, cy = 0;
	forwardSentenceEnd(&cx, &cy);
	/* Paragraph boundary should end the sentence, landing on
	 * the first non-blank line after the blank */
	TEST_ASSERT_EQUAL_INT(0, cx);
	TEST_ASSERT_EQUAL_INT(2, cy);
	destroyBuffer(buf);
}

/* ---- Kill sexp ---- */

void test_kill_sexp_parens(void) {
	struct buffer *buf = make_test_buffer("(hello) world");
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	killSexp(1);
	TEST_ASSERT_EQUAL_STRING(" world", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_kill_sexp_word(void) {
	struct buffer *buf = make_test_buffer("hello world");
	buf->cx = 0;
	buf->cy = 0;
	E.buf = buf;
	killSexp(1);
	TEST_ASSERT_EQUAL_STRING(" world", row_str(buf, 0));
	destroyBuffer(buf);
}

void test_kill_sexp_readonly(void) {
	struct buffer *buf = make_test_buffer("(hello) world");
	buf->read_only = 1;
	buf->cx = 0;
	E.buf = buf;
	killSexp(1);
	TEST_ASSERT_EQUAL_STRING("(hello) world", row_str(buf, 0));
	destroyBuffer(buf);
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
	destroyBuffer(buf);
}

void test_kill_paragraph_readonly(void) {
	const char *lines[] = { "Hello.", "", "World." };
	struct buffer *buf = make_test_buffer_lines(lines, 3);
	buf->read_only = 1;
	E.buf = buf;
	killParagraph(1);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	destroyBuffer(buf);
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
	destroyBuffer(buf);
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
	destroyBuffer(buf);
}

void test_editor_back_para(void) {
	const char *lines[] = { "A", "B", "", "C", "D" };
	struct buffer *buf = make_test_buffer_lines(lines, 5);
	buf->cx = 0;
	buf->cy = 4;
	E.buf = buf;
	backPara(1);
	TEST_ASSERT_EQUAL_INT(2, E.buf->cy);
	destroyBuffer(buf);
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_insert_char_beginning);
	RUN_TEST(test_insert_char_middle);
	RUN_TEST(test_insert_char_end);
	RUN_TEST(test_insert_char_with_count);
	RUN_TEST(test_insert_char_readonly);

	RUN_TEST(test_insert_newline_splits);
	RUN_TEST(test_insert_newline_at_beginning);
	RUN_TEST(test_insert_newline_at_end);
	RUN_TEST(test_insert_newline_and_indent);
	RUN_TEST(test_open_line);

	RUN_TEST(test_del_char_middle);
	RUN_TEST(test_del_char_joins_lines);
	RUN_TEST(test_backspace_middle);
	RUN_TEST(test_backspace_joins_lines);

	RUN_TEST(test_indent_tab);
	RUN_TEST(test_indent_spaces);
	RUN_TEST(test_unindent);

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

	return TEST_END();
}
