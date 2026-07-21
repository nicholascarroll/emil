/* test_cjk_indic.c: Tests for issues #71–#74:
 *   #71  utf8Decode + codepoint classifiers
 *   #72  CJK word movement
 *   #73  CJK and Indic sentence boundaries
 *   #74  CJK word-wrap break points */

#include "test.h"
#include "test_harness.h"
#include "unicode.h"
#include "buffer.h"
#include "motion.h"
#include "wrap.h"
#include <stdint.h>
#include <string.h>
#include <locale.h>
#include <wchar.h>

extern struct config E;

/* ================================================================
 * #71: utf8Decode returns correct codepoints
 * ================================================================ */

void test_utf8Decode_ascii(void) {
	uint8_t s[] = "$";
	TEST_ASSERT_EQUAL(0x24, (int)utf8Decode(s, 0));
}

void test_utf8Decode_2byte(void) {
	/* ¢ = U+00A2 */
	uint8_t s[] = "\xC2\xA2";
	TEST_ASSERT_EQUAL(0xA2, (int)utf8Decode(s, 0));
}

void test_utf8Decode_3byte(void) {
	/* € = U+20AC */
	uint8_t s[] = "\xE2\x82\xAC";
	TEST_ASSERT_EQUAL(0x20AC, (int)utf8Decode(s, 0));
}

void test_utf8Decode_4byte(void) {
	/* 😇 = U+1F607 */
	uint8_t s[] = "\xF0\x9F\x98\x87";
	TEST_ASSERT_EQUAL(0x1F607, (int)utf8Decode(s, 0));
}

void test_utf8Decode_cjk(void) {
	/* 中 = U+4E2D */
	uint8_t s[] = "\xE4\xB8\xAD";
	TEST_ASSERT_EQUAL(0x4E2D, (int)utf8Decode(s, 0));
}

void test_utf8Decode_at_offset(void) {
	/* "A中": decode the 中 at byte offset 1 */
	uint8_t s[] = "A\xE4\xB8\xAD";
	TEST_ASSERT_EQUAL(0x4E2D, (int)utf8Decode(s, 1));
}

/* ================================================================
 * #71: isCJKChar classifier
 * ================================================================ */

void test_isCJKChar_han(void) {
	TEST_ASSERT_TRUE(isCJKChar(0x4E2D));  /* 中 CJK Unified */
	TEST_ASSERT_TRUE(isCJKChar(0x4E00));  /* first CJK Unified */
	TEST_ASSERT_TRUE(isCJKChar(0x9FFF));  /* last CJK Unified */
}

void test_isCJKChar_hiragana(void) {
	TEST_ASSERT_TRUE(isCJKChar(0x3042));  /* あ */
	TEST_ASSERT_TRUE(isCJKChar(0x3040));  /* first Hiragana */
	TEST_ASSERT_TRUE(isCJKChar(0x309F));  /* last Hiragana */
}

void test_isCJKChar_katakana(void) {
	TEST_ASSERT_TRUE(isCJKChar(0x30A2));  /* ア */
	TEST_ASSERT_TRUE(isCJKChar(0x30A0));  /* first Katakana */
	TEST_ASSERT_TRUE(isCJKChar(0x30FF));  /* last Katakana */
}

void test_isCJKChar_hangul(void) {
	TEST_ASSERT_TRUE(isCJKChar(0xD55C));  /* 한 Hangul syllable */
	TEST_ASSERT_TRUE(isCJKChar(0xAC00));  /* first Hangul Syllables */
	TEST_ASSERT_TRUE(isCJKChar(0xD7AF));  /* last Hangul Syllables */
}

void test_isCJKChar_ext_b(void) {
	TEST_ASSERT_TRUE(isCJKChar(0x20000)); /* first CJK Extension B */
}

void test_isCJKChar_not_cjk(void) {
	TEST_ASSERT_FALSE(isCJKChar(0x41));    /* A */
	TEST_ASSERT_FALSE(isCJKChar(0xA2));    /* ¢ */
	TEST_ASSERT_FALSE(isCJKChar(0x0939));  /* ह Devanagari */
	TEST_ASSERT_FALSE(isCJKChar(0x20AC));  /* € */
}

/* ================================================================
 * #71: isCJKSentenceTerminator classifier
 * ================================================================ */

void test_cjk_sentence_terminators(void) {
	TEST_ASSERT_TRUE(isCJKSentenceTerminator(0x3002));   /* 。 */
	TEST_ASSERT_TRUE(isCJKSentenceTerminator(0xFF01));   /* ！ */
	TEST_ASSERT_TRUE(isCJKSentenceTerminator(0xFF1F));   /* ？ */
	TEST_ASSERT_FALSE(isCJKSentenceTerminator(0x002E));  /* . */
	TEST_ASSERT_FALSE(isCJKSentenceTerminator(0x4E2D));  /* 中 */
}

/* ================================================================
 * #71: isIndicSentenceTerminator classifier
 * ================================================================ */

void test_indic_sentence_terminators(void) {
	TEST_ASSERT_TRUE(isIndicSentenceTerminator(0x0964));  /* । danda */
	TEST_ASSERT_TRUE(isIndicSentenceTerminator(0x0965));  /* ॥ double danda */
	TEST_ASSERT_FALSE(isIndicSentenceTerminator(0x002E)); /* . */
	TEST_ASSERT_FALSE(isIndicSentenceTerminator(0x0939)); /* ह */
}

/* ================================================================
 * #72: CJK word movement
 * ================================================================ */

void test_forward_word_cjk_each_char_is_word(void) {
	/* 中文字 = three CJK chars, each should be one word */
	make_test_buffer("\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97");
	/*               中(3 bytes)  文(3 bytes)  字(3 bytes)    */

	forwardWord(1);
	TEST_ASSERT_EQUAL(3, E.buf->cx);  /* past 中 */

	forwardWord(1);
	TEST_ASSERT_EQUAL(6, E.buf->cx);  /* past 文 */

	forwardWord(1);
	TEST_ASSERT_EQUAL(9, E.buf->cx);  /* past 字 */
}

void test_backward_word_cjk_each_char_is_word(void) {
	/* 中文字 */
	struct buffer *buf = make_test_buffer("\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97");
	buf->cx = 9; /* end of line */

	backWord(1);
	TEST_ASSERT_EQUAL(6, E.buf->cx);  /* on 字 */

	backWord(1);
	TEST_ASSERT_EQUAL(3, E.buf->cx);  /* on 文 */

	backWord(1);
	TEST_ASSERT_EQUAL(0, E.buf->cx);  /* on 中 */
}

void test_forward_word_cjk_mixed_with_ascii(void) {
	/* "hello中文world" */
	make_test_buffer("hello\xE4\xB8\xAD\xE6\x96\x87world");

	forwardWord(1);
	TEST_ASSERT_EQUAL(5, E.buf->cx);  /* end of "hello", at 中 */

	forwardWord(1);
	TEST_ASSERT_EQUAL(8, E.buf->cx);  /* past 中 */

	forwardWord(1);
	TEST_ASSERT_EQUAL(11, E.buf->cx); /* past 文 */

	forwardWord(1);
	int row_size = E.buf->row[0].size;
	TEST_ASSERT_EQUAL(row_size, E.buf->cx); /* end of "world" */
}

void test_backward_word_cjk_mixed_with_ascii(void) {
	/* "hello中文world" */
	struct buffer *buf = make_test_buffer("hello\xE4\xB8\xAD\xE6\x96\x87world");
	buf->cx = buf->row[0].size; /* end */

	backWord(1);
	TEST_ASSERT_EQUAL(11, E.buf->cx); /* start of "world" */

	backWord(1);
	TEST_ASSERT_EQUAL(8, E.buf->cx);  /* on 文 */

	backWord(1);
	TEST_ASSERT_EQUAL(5, E.buf->cx);  /* on 中 */

	backWord(1);
	TEST_ASSERT_EQUAL(0, E.buf->cx);  /* start of "hello" */
}

void test_forward_word_latin_unchanged(void) {
	/* Ensure normal Latin word movement still works */
	make_test_buffer("hello world");

	forwardWord(1);
	TEST_ASSERT_EQUAL(5, E.buf->cx);  /* at space after "hello" */

	forwardWord(1);
	TEST_ASSERT_EQUAL(11, E.buf->cx); /* end of "world" */
}

/* ================================================================
 * #73: CJK sentence boundaries
 * ================================================================ */

void test_forward_sentence_cjk(void) {
	/* 你好。世界！再见 */
	struct buffer *buf = make_test_buffer(
		"\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82"
		"\xE4\xB8\x96\xE7\x95\x8C\xEF\xBC\x81"
		"\xE5\x86\x8D\xE8\xA7\x81");
	/* 你(3) 好(3) 。(3) 世(3) 界(3) ！(3) 再(3) 见(3) = 24 bytes */
	(void)buf;

	forwardSentence(1);
	/* Should land after 。at byte 9 */
	TEST_ASSERT_EQUAL(9, E.buf->cx);

	forwardSentence(1);
	/* Should land after ！ at byte 18 */
	TEST_ASSERT_EQUAL(18, E.buf->cx);
}

void test_backward_sentence_cjk(void) {
	/* 你好。世界 */
	struct buffer *buf = make_test_buffer(
		"\xE4\xBD\xA0\xE5\xA5\xBD\xE3\x80\x82"
		"\xE4\xB8\x96\xE7\x95\x8C");
	/* 你(3) 好(3) 。(3) 世(3) 界(3) = 15 bytes */
	buf->cx = 15;

	backwardSentence(1);
	/* Should land at byte 9 (after 。, start of 世) */
	TEST_ASSERT_EQUAL(9, E.buf->cx);
}

void test_forward_sentence_indic(void) {
	/* Simple Indic: नमस्ते। दुनिया
	 * danda at U+0964 marks sentence end */
	/* नमस्ते = E0 A4 A8 E0 A4 AE E0 A4 B8 E0 A5 8D E0 A4 A4 E0 A5 87 (18 bytes)
	 * ।      = E0 A5 A4 (3 bytes)
	 * space  = 20 (1 byte)
	 * द      = E0 A4 A6 (3 bytes) ... */
	make_test_buffer("\xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D"
		 "\xE0\xA4\xA4\xE0\xA5\x87\xE0\xA5\xA4 "
		 "\xE0\xA4\xA6\xE0\xA5\x81\xE0\xA4\xA8\xE0\xA4\xBF"
		 "\xE0\xA4\xAF\xE0\xA4\xBE");
	E.buf->cx = 0;

	forwardSentence(1);
	/* Should land after the danda at byte 21 */
	TEST_ASSERT_EQUAL(21, E.buf->cx);
}

void test_forward_sentence_latin_unchanged(void) {
	/* Ensure the existing Latin sentence detection still works */
	make_test_buffer("Hello world. Goodbye world.");
	E.buf->cx = 0;

	forwardSentence(1);
	/* Should land at the space after '.' (byte 12) */
	TEST_ASSERT_EQUAL(12, E.buf->cx);
}

/* ================================================================
 * #74: CJK word-wrap break points
 * ================================================================ */

void test_wordwrap_cjk_breaks_between_chars(void) {
	/* 6 CJK chars, each 2 columns wide = 12 columns total.
	 * With screencols=8, should break after 4th char (8 cols). */
	uint8_t text[] = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97"
			 "\xE6\xB5\x8B\xE8\xAF\x95\xE5\x93\x81";
	/* 中文字测试品 = 18 bytes, 12 display columns */
	erow row = {0};
	row.chars = text;
	row.size = 18;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 8, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	TEST_ASSERT_EQUAL(8, break_col);   /* 4 CJK chars × 2 cols */
	TEST_ASSERT_EQUAL(12, break_byte); /* 4 CJK chars × 3 bytes */
}

void test_wordwrap_cjk_no_hard_break(void) {
	/* Without the CJK fix, CJK text would hard-break because
	 * isWordBoundary never matches — verify we get a soft break. */
	uint8_t text[] = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97"
			 "\xE6\xB5\x8B";
	/* 中文字测 = 12 bytes, 8 display columns */
	erow row = {0};
	row.chars = text;
	row.size = 12;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 6, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	/* Should break after 3rd CJK char (6 cols, 9 bytes) */
	TEST_ASSERT_EQUAL(6, break_col);
	TEST_ASSERT_EQUAL(9, break_byte);
}

void test_wordwrap_ascii_unchanged(void) {
	/* Ensure normal ASCII word wrap still works */
	uint8_t text[] = "hello world foo";
	erow row = {0};
	row.chars = text;
	row.size = strlen((char *)text);
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 12, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	/* Should break at the space after "world" (col 12, byte 12) */
	TEST_ASSERT_EQUAL(12, break_col);
	TEST_ASSERT_EQUAL(12, break_byte);
}

/* ================================================================ */

void setUp(void) {
	initTestEditor();
}

void tearDown(void) {
	cleanupTestEditor();
}

/* ---- 行首禁则: closing punctuation forbidden at line start ---- */

/* A break that would put 。 at the start of the next line must move
 * back one character, carrying 字。 over together. */
void test_wordwrap_no_leading_close_punct(void) {
	/* 中文字。测试: 18 bytes, cols=7: 中文字 fits (6 cols) but
	 * the break after 字 is suppressed (。 would lead), so the
	 * line breaks after 文. */
	uint8_t text[] = "\xE4\xB8\xAD\xE6\x96\x87\xE5\xAD\x97"
			 "\xE3\x80\x82\xE6\xB5\x8B\xE8\xAF\x95";
	erow row = { 0 };
	row.chars = text;
	row.size = 18;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 7, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	TEST_ASSERT_EQUAL(4, break_col);  /* after 文 */
	TEST_ASSERT_EQUAL(6, break_byte); /* 字。测试 wraps together */
}

/* The ideal break is right AFTER closing punctuation: 。 is itself a
 * break-after candidate. */
void test_wordwrap_break_after_close_punct(void) {
	/* 中文。字词: cols=6: 中文。 exactly fills the line and the
	 * break lands after 。, not back at 中|文. */
	uint8_t text[] = "\xE4\xB8\xAD\xE6\x96\x87\xE3\x80\x82"
			 "\xE5\xAD\x97\xE8\xAF\x8D";
	erow row = { 0 };
	row.chars = text;
	row.size = 15;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 6, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	TEST_ASSERT_EQUAL(6, break_col);  /* after 。 */
	TEST_ASSERT_EQUAL(9, break_byte); /* next line: 字词 */
}

/* Chained forbidden characters (」。) all travel together. */
void test_wordwrap_close_punct_chain(void) {
	/* 文字」。后: cols=6: breaks after 字 and after 」 are both
	 * suppressed, so the line breaks after 文 and 字」。 wrap as
	 * a unit. */
	uint8_t text[] = "\xE6\x96\x87\xE5\xAD\x97\xE3\x80\x8D"
			 "\xE3\x80\x82\xE5\x90\x8E";
	erow row = { 0 };
	row.chars = text;
	row.size = 15;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 6, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	TEST_ASSERT_EQUAL(2, break_col);  /* after 文 */
	TEST_ASSERT_EQUAL(3, break_byte); /* next line: 字」。后 */
}

/* A line of pure closing punctuation suppresses every candidate; the
 * hard-break fallback then applies (a forbidden character DOES start
 * the next line here, by design — better than an empty line or a
 * loop). */
void test_wordwrap_all_forbidden_fallback(void) {
	/* 。。。。: cols=4: two fit, hard break before the third. */
	uint8_t text[] = "\xE3\x80\x82\xE3\x80\x82\xE3\x80\x82"
			 "\xE3\x80\x82";
	erow row = { 0 };
	row.chars = text;
	row.size = 12;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 4, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	TEST_ASSERT_EQUAL(4, break_col);
	TEST_ASSERT_EQUAL(6, break_byte);
}

/* The rule also guards ASCII space breaks: a space followed by 。 is
 * not a break candidate, and the 。 (fitting at end of line) becomes
 * the break point instead. */
void test_wordwrap_space_before_close_punct(void) {
	/* "ab cd 。ef": cols=8: break after the first space is
	 * recorded, after the second suppressed, then 。 fits at
	 * column 8 and the break lands after it. */
	uint8_t text[] = "ab cd \xE3\x80\x82"
			 "ef";
	erow row = { 0 };
	row.chars = text;
	row.size = 11;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 8, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	TEST_ASSERT_EQUAL(8, break_col);  /* after 。 */
	TEST_ASSERT_EQUAL(9, break_byte); /* next line: "ef" */
}

/* ---- Thai/Lao/Khmer boundaries ---- */

/* Khmer sentences end at ។ (KHAN); sentence motion must recognise it
 * in both directions. */
void test_sentence_khmer_khan(void) {
	/* កខ។គឃ: terminator ។ at bytes 6..8 */
	struct buffer *buf = make_test_buffer(
		"\xE1\x9E\x80\xE1\x9E\x81\xE1\x9F\x94"
		"\xE1\x9E\x82\xE1\x9E\x83");
	(void)buf;

	int cx = 0, cy = 0;
	TEST_ASSERT_EQUAL_INT(0, forwardSentenceEnd(&cx, &cy));
	TEST_ASSERT_EQUAL_INT(9, cx); /* just past ។ */
	TEST_ASSERT_EQUAL_INT(0, cy);

	cx = 15;
	cy = 0;
	TEST_ASSERT_EQUAL_INT(0, backwardSentenceStart(&cx, &cy));
	TEST_ASSERT_EQUAL_INT(9, cx); /* sentence starts after ។ */
	TEST_ASSERT_EQUAL_INT(0, cy);
}

/* ZERO WIDTH SPACE is the explicit word separator of digital
 * Thai/Lao/Khmer text: word wrap must treat it as a break
 * opportunity. */
void test_wordwrap_zwsp_break(void) {
	/* กขค[ZWSP]งจฉ: cols=4: break falls at the ZWSP (consumed at
	 * end of line 1), next line starts งจฉ. */
	uint8_t text[] = "\xE0\xB8\x81\xE0\xB8\x82\xE0\xB8\x84"
			 "\xE2\x80\x8B"
			 "\xE0\xB8\x87\xE0\xB8\x88\xE0\xB8\x89";
	erow row = { 0 };
	row.chars = text;
	row.size = 21;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 4, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	TEST_ASSERT_EQUAL(3, break_col);   /* ZWSP itself is width 0 */
	TEST_ASSERT_EQUAL(12, break_byte); /* just past the ZWSP */
}

/* Word motion honours ZWSP as a boundary in both directions. */
void test_word_motion_zwsp(void) {
	/* กข[ZWSP]งจ */
	struct buffer *buf = make_test_buffer(
		"\xE0\xB8\x81\xE0\xB8\x82"
		"\xE2\x80\x8B"
		"\xE0\xB8\x87\xE0\xB8\x88");

	buf->cx = 0;
	buf->cy = 0;
	int dx, dy;
	forwardWordEnd(&dx, &dy);
	TEST_ASSERT_EQUAL_INT(6, dx); /* stops at the ZWSP */
	TEST_ASSERT_EQUAL_INT(0, dy);

	buf->cx = 15;
	buf->cy = 0;
	backwardWordEnd(&dx, &dy);
	TEST_ASSERT_EQUAL_INT(9, dx); /* stops just after the ZWSP */
	TEST_ASSERT_EQUAL_INT(0, dy);
}

/* Preposed vowels (เ แ โ ใ ไ) are written before their consonant; the
 * hard-break fallback must not strand one at end of line. */
void test_wordwrap_no_split_after_preposed(void) {
	/* กขคเงจ: cols=4: the raw hard break would fall between เ
	 * and ง; it retreats to before เ instead. */
	uint8_t text[] = "\xE0\xB8\x81\xE0\xB8\x82\xE0\xB8\x84"
			 "\xE0\xB9\x80"
			 "\xE0\xB8\x87\xE0\xB8\x88";
	erow row = { 0 };
	row.chars = text;
	row.size = 18;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 4, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	TEST_ASSERT_EQUAL(3, break_col); /* before เ */
	TEST_ASSERT_EQUAL(9, break_byte); /* เงจ wraps together */
}

/* A hard break can never separate a base character from a following
 * combining mark: zero-width marks always "fit", so the break lands
 * after the whole cluster. */
void test_wordwrap_combining_cluster_intact(void) {
	/* กขกิค: cols=3: line 1 is exactly กขกิ (the SARA I stays
	 * glued to its base), line 2 is ค. */
	uint8_t text[] = "\xE0\xB8\x81\xE0\xB8\x82\xE0\xB8\x81"
			 "\xE0\xB8\xB4"
			 "\xE0\xB8\x84";
	erow row = { 0 };
	row.chars = text;
	row.size = 15;
	row.cached_width = -1;

	int break_col, break_byte;
	int more = wordWrapBreak(&row, 3, 0, 0, &break_col, &break_byte);

	TEST_ASSERT_TRUE(more);
	TEST_ASSERT_EQUAL(3, break_col);
	TEST_ASSERT_EQUAL(12, break_byte); /* after กิ, before ค */
}

int main(void) {
	setlocale(LC_CTYPE, "C.UTF-8");

	TEST_BEGIN();

	/* #71: utf8Decode */
	RUN_TEST(test_utf8Decode_ascii);
	RUN_TEST(test_utf8Decode_2byte);
	RUN_TEST(test_utf8Decode_3byte);
	RUN_TEST(test_utf8Decode_4byte);
	RUN_TEST(test_utf8Decode_cjk);
	RUN_TEST(test_utf8Decode_at_offset);

	/* #71: classifiers */
	RUN_TEST(test_isCJKChar_han);
	RUN_TEST(test_isCJKChar_hiragana);
	RUN_TEST(test_isCJKChar_katakana);
	RUN_TEST(test_isCJKChar_hangul);
	RUN_TEST(test_isCJKChar_ext_b);
	RUN_TEST(test_isCJKChar_not_cjk);
	RUN_TEST(test_cjk_sentence_terminators);
	RUN_TEST(test_indic_sentence_terminators);

	/* #72: CJK word movement */
	RUN_TEST(test_forward_word_cjk_each_char_is_word);
	RUN_TEST(test_backward_word_cjk_each_char_is_word);
	RUN_TEST(test_forward_word_cjk_mixed_with_ascii);
	RUN_TEST(test_backward_word_cjk_mixed_with_ascii);
	RUN_TEST(test_forward_word_latin_unchanged);

	/* #73: CJK and Indic sentence boundaries */
	RUN_TEST(test_forward_sentence_cjk);
	RUN_TEST(test_backward_sentence_cjk);
	RUN_TEST(test_forward_sentence_indic);
	RUN_TEST(test_forward_sentence_latin_unchanged);

	/* #74: CJK word-wrap */
	RUN_TEST(test_wordwrap_cjk_breaks_between_chars);
	RUN_TEST(test_wordwrap_cjk_no_hard_break);
	RUN_TEST(test_wordwrap_no_leading_close_punct);
	RUN_TEST(test_wordwrap_break_after_close_punct);
	RUN_TEST(test_wordwrap_close_punct_chain);
	RUN_TEST(test_wordwrap_all_forbidden_fallback);
	RUN_TEST(test_wordwrap_space_before_close_punct);
	RUN_TEST(test_sentence_khmer_khan);
	RUN_TEST(test_wordwrap_zwsp_break);
	RUN_TEST(test_word_motion_zwsp);
	RUN_TEST(test_wordwrap_no_split_after_preposed);
	RUN_TEST(test_wordwrap_combining_cluster_intact);
	RUN_TEST(test_wordwrap_ascii_unchanged);

	return TEST_END();
}
