#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <sys/types.h>
#include <stdio.h>
#include <stdint.h>
#include <wchar.h>
#include "unicode.h"
#include "emil.h"

/* Decode the UTF-8 character at str[idx] and return its Unicode codepoint. */
uint32_t utf8Decode(const uint8_t *str, int idx) {
	/* Each continuation byte is verified before the byte after it
	 * is read.  Rows are NUL-terminated at chars[size], and NUL is
	 * never a continuation byte, so decoding stops at the
	 * terminator for truncated sequences (which can reach a buffer
	 * via byte-column rectangle operations on multibyte text)
	 * instead of reading past the allocation.  An invalid sequence
	 * decodes as its lead byte, matching the pre-existing handling
	 * of stray non-UTF-8 bytes. */
	uint32_t ret = 0;
	uint8_t ch = str[idx];
	if (utf8_is2Char(ch)) {
		if ((str[idx + 1] & 0xC0) != 0x80)
			return ch;
		ret = (ch & 0x1F) << 6;
		ret |= (str[idx + 1] & 0x3F);
	} else if (utf8_is3Char(ch)) {
		if ((str[idx + 1] & 0xC0) != 0x80 ||
		    (str[idx + 2] & 0xC0) != 0x80)
			return ch;
		ret = (ch & 0x0F) << 12;
		ret |= ((str[idx + 1] & 0x3F) << 6);
		ret |= (str[idx + 2] & 0x3F);
	} else if (utf8_is4Char(ch)) {
		if ((str[idx + 1] & 0xC0) != 0x80 ||
		    (str[idx + 2] & 0xC0) != 0x80 ||
		    (str[idx + 3] & 0xC0) != 0x80)
			return ch;
		ret = (ch & 0x07) << 18;
		ret |= ((str[idx + 1] & 0x3F) << 12);
		ret |= ((str[idx + 2] & 0x3F) << 6);
		ret |= (str[idx + 3] & 0x3F);
	} else {
		ret = str[idx];
	}
	return ret;
}

/* Convert a 32 bit value to UTF-8, assuming that dest is big enough
 * to store it. returns number of bytes (1-4) written. */
static ssize_t rune_to_utf8(uint8_t *dest, uint32_t ru) {
	/*
	 * for continuation bytes
	 * 00111111 = 3F
	 * 10000000 = 80
	 * 10111111 = BF
	 *
	 * for 2-bytes
	 * 00011111 = 1F
	 * 11000000 = C0
	 * 11011111 = DF
	 *
	 * for 3-bytes
	 * 00001111 = 0F
	 * 11100000 = E0
	 * 11101111 = EF
	 *
	 * for 4-bytes
	 * 00000111 = 07
	 * 11110000 = F0
	 * 11110111 = F7
	 */
	if (ru < 0x80) {
		/* ASCII */
		dest[0] = (uint8_t)ru;
		return 1;
	} else if (ru < 0x0800) {
		/* 2 bytes */
		dest[0] = ((uint8_t)(ru >> 6) & 0x1F) | 0xC0;
		dest[1] = ((uint8_t)ru & 0x3F) | 0x80;
		return 2;
	} else if (ru < 0x10000) {
		/* 3 bytes */
		dest[0] = ((uint8_t)(ru >> 12) & 0x0F) | 0xE0;
		dest[1] = ((uint8_t)(ru >> 6) & 0x3F) | 0x80;
		dest[2] = ((uint8_t)ru & 0x3F) | 0x80;
		return 3;
	} else {
		/* 4 bytes */
		dest[0] = ((uint8_t)(ru >> 18) & 0x07) | 0xF0;
		dest[1] = ((uint8_t)(ru >> 12) & 0x3F) | 0x80;
		dest[2] = ((uint8_t)(ru >> 6) & 0x3F) | 0x80;
		dest[3] = ((uint8_t)ru & 0x3F) | 0x80;
		return 4;
	}
}

static int testCaseUCS(char *testCh, int expected) {
	int ucs = utf8Decode((const uint8_t *)testCh, 0);
	printf("%s\tgot %04x\texpected %04x\n", testCh, ucs, expected);
	return expected != ucs;
}

static int testCaseReverseUCS(char *expectedChars, int expectedWidth,
			      int input) {
	uint8_t result[4];
	ssize_t actualWidth;
	int resultsNotMatch = 0;
	int i = 0;

	actualWidth = rune_to_utf8(result, input);

	while (expectedChars[i] != 0) {
		printf("%i actual: %02x expected: %02x\n", i, result[i],
		       (uint8_t)expectedChars[i]);
		resultsNotMatch += result[i] != (uint8_t)expectedChars[i];
		i++;
	}
	printf("expected width %i actual %zd\n", expectedWidth, actualWidth);

	return (actualWidth != expectedWidth) + resultsNotMatch;
}

static int testCaseStringWidth(char *str, int expected) {
	int actual = stringWidth((const uint8_t *)str);
	printf("%s\tgot %i\texpected %i\n", str, actual, expected);
	return actual != expected;
}

int unicodeTest(void) {
	printf("UTF8 -> UCS conversion test\n");
	int retval = testCaseUCS("$", 0x24);
	retval = retval + testCaseUCS("\xC2\xA2", 0xA2);
	retval = retval + testCaseUCS("\xE0\xA4\xB9", 0x939);
	retval = retval + testCaseUCS("\xE2\x82\xAC", 0x20AC);
	retval = retval + testCaseUCS("\xED\x95\x9C", 0xD55C);
	retval = retval + testCaseUCS("\xF0\x90\x8D\x88", 0x10348);
	retval = retval + testCaseUCS("\xF0\x9f\x98\x87", 0x1f607);
	printf("Rune width test\n");
	retval += testCaseStringWidth("bruh", 4);
	retval += testCaseStringWidth("生存戦略", 8);
	retval += testCaseStringWidth("😇", 2);
	printf("UCS -> UTF8 conversion test\n");
	retval = retval + testCaseReverseUCS("\xC2\xA2", 2, 0xA2);
	retval = retval + testCaseReverseUCS("\xE0\xA4\xB9", 3, 0x939);
	retval = retval + testCaseReverseUCS("\xE2\x82\xAC", 3, 0x20AC);
	retval = retval + testCaseReverseUCS("\xED\x95\x9C", 3, 0xD55C);
	retval = retval + testCaseReverseUCS("\xF0\x90\x8D\x88", 4, 0x10348);
	retval = retval + testCaseReverseUCS("\xF0\x9f\x98\x87", 4, 0x1f607);
	return retval;
}

int stringWidth(const uint8_t *str) {
	int idx = 0;
	int width = 0;

	while (str[idx] != 0) {
		width += charInStringWidth(str, idx);
		idx += utf8_nBytes(str[idx]);
	}

	return width;
}

int charInStringWidth(const uint8_t *str, int idx) {
	if (str[idx] < 0x20) {
		return 2;
	} else if (str[idx] < 0x7f) {
		return 1;
	} else if (str[idx] == 0x7f) {
		/* The canonical way to display DEL is ^? */
		return 2;
	} else {
		int rune = utf8Decode(str, idx);
		int w = wcwidth((wchar_t)rune);
		return w < 0 ? 1 : w;
	}
}

int utf8_is2Char(uint8_t ch) {
	return (0xC2 <= ch && ch <= 0xDF);
}

int utf8_is3Char(uint8_t ch) {
	return (0xE0 <= ch && ch <= 0xEF);
}

int utf8_is4Char(uint8_t ch) {
	return (0xF0 <= ch && ch <= 0xF4);
}

int utf8_nBytes(uint8_t ch) {
	if (ch < 0x80) {
		return 1;
	} else if (utf8_is4Char(ch)) {
		return 4;
	} else if (utf8_is3Char(ch)) {
		return 3;
	} else if (utf8_is2Char(ch)) {
		return 2;
	} else {
		return 1;
	}
}

int utf8_isCont(uint8_t ch) {
	return (0x80 <= ch && ch <= 0xBF);
}

/* CJK character classifier.  Returns 1 if cp is a CJK ideograph,
 * Hiragana, Katakana, Hangul syllable, or Hangul Jamo, i.e. a
 * character that functions as its own word and is a valid line-break
 * point in CJK typesetting. */
int isCJKChar(uint32_t cp) {
	return (cp >= 0x4E00 && cp <= 0x9FFF)	   /* CJK Unified Ideographs */
	       || (cp >= 0x3400 && cp <= 0x4DBF)   /* CJK Extension A */
	       || (cp >= 0x20000 && cp <= 0x2A6DF) /* CJK Extension B */
	       || (cp >= 0x2A700 && cp <= 0x2B73F) /* CJK Extension C */
	       || (cp >= 0x2B740 && cp <= 0x2B81F) /* CJK Extension D */
	       || (cp >= 0x2B820 && cp <= 0x2CEAF) /* CJK Extension E */
	       || (cp >= 0x2CEB0 && cp <= 0x2EBEF) /* CJK Extension F */
	       || (cp >= 0x30000 && cp <= 0x3134F) /* CJK Extension G */
	       ||
	       (cp >= 0xF900 && cp <= 0xFAFF) /* CJK Compatibility Ideographs */
	       || (cp >= 0x3040 && cp <= 0x309F) /* Hiragana */
	       || (cp >= 0x30A0 && cp <= 0x30FF) /* Katakana */
	       ||
	       (cp >= 0x31F0 && cp <= 0x31FF) /* Katakana Phonetic Extensions */
	       || (cp >= 0xAC00 && cp <= 0xD7AF) /* Hangul Syllables */
	       || (cp >= 0x1100 && cp <= 0x11FF) /* Hangul Jamo */
	       || (cp >= 0x3130 && cp <= 0x318F) /* Hangul Compatibility Jamo */
	       || (cp >= 0xA960 && cp <= 0xA97F) /* Hangul Jamo Extended-A */
	       || (cp >= 0xD7B0 && cp <= 0xD7FF); /* Hangul Jamo Extended-B */
}

/* 行首禁则 — characters forbidden at the start of a wrapped line.
 * Initial set: closing punctuation that must stay attached to the
 * character it follows.  Word wrap consults this to avoid recording
 * a break point immediately before any of these.  Extend the table
 * as needed (e.g. " ' 】 〉 〕 are natural future members). */
int isLineStartForbidden(uint32_t cp) {
	switch (cp) {
	case 0x3001: /* 、 IDEOGRAPHIC COMMA */
	case 0x3002: /* 。 IDEOGRAPHIC FULL STOP */
	case 0xFF0C: /* ， FULLWIDTH COMMA */
	case 0xFF01: /* ！ FULLWIDTH EXCLAMATION MARK */
	case 0xFF1F: /* ？ FULLWIDTH QUESTION MARK */
	case 0xFF1A: /* ： FULLWIDTH COLON */
	case 0xFF1B: /* ； FULLWIDTH SEMICOLON */
	case 0xFF09: /* ） FULLWIDTH RIGHT PARENTHESIS */
	case 0x300D: /* 」 RIGHT CORNER BRACKET */
	case 0x300B: /* 》 RIGHT DOUBLE ANGLE BRACKET */
	case 0xFF3D: /* ］ FULLWIDTH RIGHT SQUARE BRACKET */
		return 1;
	default:
		return 0;
	}
}

/* CJK sentence terminators: 。(U+3002) ！(U+FF01) ？(U+FF1F) */
int isCJKSentenceTerminator(uint32_t cp) {
	return cp == 0x3002 || cp == 0xFF01 || cp == 0xFF1F;
}

/* Southeast Asian sentence terminators: Khmer ។ (U+17D4 KHAN, full
 * stop) and ៕ (U+17D5 BARIYOOSAN, end of section); Thai ๚ (U+0E5A
 * ANGKHANKHU) and ๛ (U+0E5B KHOMUT) for classical texts.  Modern
 * Thai and Lao mark sentence ends with spaces only — inherently
 * ambiguous without a dictionary — so those fall back to the
 * end-of-line invariant in sentence motion. */
int isSEAsianSentenceTerminator(uint32_t cp) {
	return cp == 0x17D4 || cp == 0x17D5 || cp == 0x0E5A || cp == 0x0E5B;
}

/* Explicit word-separator codepoints beyond ASCII.  ZERO WIDTH SPACE
 * is the standard way digital Thai/Lao/Khmer text marks word breaks
 * in otherwise unspaced runs; word motion treats it as a boundary
 * and word wrap treats it as a break opportunity. */
int isWordSeparatorCP(uint32_t cp) {
	return cp == 0x200B;
}

/* Preposed vowels are spacing characters written BEFORE the
 * consonant they modify (Thai เ แ โ ใ ไ, Lao ເ ແ ໂ ໃ ໄ).  A line
 * break between the vowel and its consonant is visually wrong, so
 * the word-wrap hard-break fallback refuses to split there. */
int isPreposedVowel(uint32_t cp) {
	return (cp >= 0x0E40 && cp <= 0x0E44) || (cp >= 0x0EC0 && cp <= 0x0EC4);
}

/* Indic sentence terminators: danda । (U+0964) and double danda ॥ (U+0965) */
int isIndicSentenceTerminator(uint32_t cp) {
	return cp == 0x0964 || cp == 0x0965;
}

/*
 * Validate a UTF-8 byte sequence.
 *
 * Returns 1 if buf[0..len-1] is valid UTF-8, 0 otherwise.
 * Checks continuation bytes, rejects overlong encodings,
 * surrogate halves (U+D800..U+DFFF), null bytes, and
 * codepoints above U+10FFFF.
 */
int utf8_validate(const uint8_t *buf, int len) {
	int i = 0;
	while (i < len) {
		uint8_t c = buf[i];

		if (c == 0x00) {
			return 0;
		} else if (c <= 0x7F) {
			i++;
		} else if ((c & 0xE0) == 0xC0) {
			if (c < 0xC2)
				return 0; /* Overlong */
			if (i + 1 >= len || (buf[i + 1] & 0xC0) != 0x80)
				return 0;
			i += 2;
		} else if ((c & 0xF0) == 0xE0) {
			if (i + 2 >= len || (buf[i + 1] & 0xC0) != 0x80 ||
			    (buf[i + 2] & 0xC0) != 0x80)
				return 0;
			unsigned int cp = ((c & 0x0F) << 12) |
					  ((buf[i + 1] & 0x3F) << 6) |
					  (buf[i + 2] & 0x3F);
			if (cp < 0x800)
				return 0; /* Overlong */
			if (cp >= 0xD800 && cp <= 0xDFFF)
				return 0; /* Surrogate */
			i += 3;
		} else if ((c & 0xF8) == 0xF0) {
			if (c > 0xF4)
				return 0; /* Above U+10FFFF */
			if (i + 3 >= len || (buf[i + 1] & 0xC0) != 0x80 ||
			    (buf[i + 2] & 0xC0) != 0x80 ||
			    (buf[i + 3] & 0xC0) != 0x80)
				return 0;
			unsigned int cp = ((c & 0x07) << 18) |
					  ((buf[i + 1] & 0x3F) << 12) |
					  ((buf[i + 2] & 0x3F) << 6) |
					  (buf[i + 3] & 0x3F);
			if (cp < 0x10000)
				return 0; /* Overlong */
			if (cp > 0x10FFFF)
				return 0; /* Above Unicode max */
			i += 4;
		} else {
			return 0; /* Invalid lead byte */
		}
	}
	return 1;
}

int nextScreenX(uint8_t *str, int *idx, int screen_x) {
	uint8_t ch = str[*idx];

	if (ch == '\t') {
		/* Move to next tab stop */
		screen_x = ((screen_x / EMIL_TAB_STOP) + 1) * EMIL_TAB_STOP;
	} else if (ch < 0x20 || ch == 0x7f) {
		/* Control characters display as ^X */
		screen_x += 2;
	} else if (ch < 0x80) {
		/* Regular ASCII */
		screen_x += 1;
	} else {
		/* Unicode character */
		int width = charInStringWidth(str, *idx);
		screen_x += width;
		/* Skip continuation bytes */
		int nbytes = utf8_nBytes(ch);
		*idx += nbytes - 1;
	}

	return screen_x;
}
