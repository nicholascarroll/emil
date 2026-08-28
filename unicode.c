/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include <sys/types.h>
#include <stdio.h>
#include <stdint.h>
#include <stddef.h>
#include <wchar.h>
#include "unicode.h"
#include "emil.h"

struct known_script_range {
	uint32_t start;
	uint32_t end;
	const char *name;
};

/* This does not pretend to be the full list of scripts in the Unicode specification.*/
static const struct known_script_range known_scripts[] = {
	/* European & Middle Eastern */
	{ 0x0041, 0x005A, "Latin" }, /* A-Z */
	{ 0x0061, 0x007A, "Latin" }, /* a-z */
	{ 0x00C0, 0x024F, "Latin" }, /* Latin-1 Supplement, Extended-A/B */
	{ 0x1E00, 0x1EFF, "Latin" }, /* Latin Extended Additional */
	{ 0x2C60, 0x2C7F, "Latin" }, /* Latin Extended-C */
	{ 0xA720, 0xA7FF, "Latin" }, /* Latin Extended-D */
	{ 0xFB00, 0xFB06, "Latin" }, /* Latin ligatures */
	{ 0xFF21, 0xFF3A, "Latin" }, /* Fullwidth A-Z */
	{ 0xFF41, 0xFF5A, "Latin" }, /* Fullwidth a-z */

	{ 0x0400, 0x04FF, "Cyrillic" },
	{ 0x0500, 0x052F, "Cyrillic" }, /* Cyrillic Supplement */
	{ 0x2DE0, 0x2DFF, "Cyrillic" }, /* Cyrillic Extended-A */
	{ 0xA640, 0xA69F, "Cyrillic" }, /* Cyrillic Extended-B */

	{ 0x0370, 0x03FF, "Greek" },
	{ 0x1F00, 0x1FFF, "Greek" }, /* Greek Extended */

	{ 0x0530, 0x058F, "Armenian" },
	{ 0xFB13, 0xFB17, "Armenian" }, /* Armenian ligatures */

	{ 0x10A0, 0x10FF, "Georgian" },
	{ 0x2D00, 0x2D2F, "Georgian" }, /* Georgian Supplement */

	{ 0x16A0, 0x16FF, "Runic" },
	{ 0x1680, 0x169F, "Ogham" },

	{ 0x0600, 0x06FF, "Arabic" },
	{ 0x0750, 0x077F, "Arabic" }, /* Arabic Supplement */
	{ 0x08A0, 0x08FF, "Arabic" }, /* Arabic Extended-A */
	{ 0xFB50, 0xFDFF, "Arabic" }, /* Arabic Presentation Forms-A */
	{ 0xFE70, 0xFEFF, "Arabic" }, /* Arabic Presentation Forms-B */

	{ 0x0590, 0x05FF, "Hebrew" },
	{ 0xFB1D, 0xFB4F, "Hebrew" }, /* Hebrew Presentation Forms */

	/* Chinese & East Asian */
	{ 0x2E80, 0x2EFF, "Han" },   /* CJK Radicals Supplement */
	{ 0x2F00, 0x2FDF, "Han" },   /* Kangxi Radicals */
	{ 0x3400, 0x4DBF, "Han" },   /* CJK Extension A */
	{ 0x4E00, 0x9FFF, "Han" },   /* CJK Unified Ideographs */
	{ 0xF900, 0xFAFF, "Han" },   /* CJK Compatibility Ideographs */
	{ 0x20000, 0x2A6DF, "Han" }, /* CJK Extension B */
	{ 0x2A700, 0x2B73F, "Han" }, /* CJK Extension C */
	{ 0x2B740, 0x2B81F, "Han" }, /* CJK Extension D */
	{ 0x2B820, 0x2CEAF, "Han" }, /* CJK Extension E */
	{ 0x2CEB0, 0x2EBEF, "Han" }, /* CJK Extension F */
	{ 0x2F800, 0x2FA1F, "Han" }, /* CJK Compatibility Supplement */
	{ 0x30000, 0x3134F, "Han" }, /* CJK Extension G */

	{ 0x02EA, 0x02EB, "Bopomofo" }, /* Bopomofo Extended */
	{ 0x3100, 0x312F, "Bopomofo" },
	{ 0x31A0, 0x31BF, "Bopomofo" }, /* Bopomofo Extended */

	{ 0x1100, 0x11FF, "Hangul" }, /* Hangul Jamo */
	{ 0x3130, 0x318F, "Hangul" }, /* Hangul Compatibility Jamo */
	{ 0xA960, 0xA97F, "Hangul" }, /* Hangul Jamo Extended-A */
	{ 0xAC00, 0xD7AF, "Hangul" }, /* Hangul Syllables */
	{ 0xD7B0, 0xD7FF, "Hangul" }, /* Hangul Jamo Extended-B */

	{ 0x3040, 0x309F, "Hiragana" },
	{ 0x1B001, 0x1B11F, "Hiragana" }, /* Hiragana Extended-A */

	{ 0x30A0, 0x30FF, "Katakana" },
	{ 0x31F0, 0x31FF, "Katakana" },	  /* Katakana Phonetic Extensions */
	{ 0xFF66, 0xFF9D, "Katakana" },	  /* Halfwidth Katakana */
	{ 0x1B120, 0x1B122, "Katakana" }, /* Katakana Extended-A */

	/* South Asian / Indic */
	{ 0x0900, 0x097F, "Devanagari" },
	{ 0xA8E0, 0xA8FF, "Devanagari" }, /* Devanagari Extended */

	{ 0x0980, 0x09FF, "Bengali" },
	{ 0x0A00, 0x0A7F, "Gurmukhi" },
	{ 0x0A80, 0x0AFF, "Gujarati" },
	{ 0x0B00, 0x0B7F, "Oriya" },
	{ 0x0B80, 0x0BFF, "Tamil" },
	{ 0x0C00, 0x0C7F, "Telugu" },
	{ 0x0C80, 0x0CFF, "Kannada" },
	{ 0x0D00, 0x0D7F, "Malayalam" },
	{ 0x0D80, 0x0DFF, "Sinhala" },

	/* Southeast Asian */
	{ 0x0E00, 0x0E7F, "Thai" },
	{ 0x0E80, 0x0EFF, "Lao" },
	{ 0x1780, 0x17FF, "Khmer" },
	{ 0x19E0, 0x19FF, "Khmer" }, /* Khmer Symbols */
	{ 0x1000, 0x109F, "Myanmar" },
	{ 0xA9E0, 0xA9FE, "Myanmar" }, /* Myanmar Extended-B */
	{ 0xAA60, 0xAA7F, "Myanmar" }, /* Myanmar Extended-A */

	/* Other Major Regional */
	{ 0xA000, 0xA4CF, "Yi" },
	{ 0x1800, 0x18AF, "Mongolian" },
	{ 0x11660, 0x1167F, "Mongolian" }, /* Mongolian Supplement */
	{ 0x0F00, 0x0FFF, "Tibetan" },
	{ 0x1700, 0x171F, "Tagalog" },
	{ 0x1200, 0x137F, "Ethiopic" },
	{ 0x1380, 0x139F, "Ethiopic" },	  /* Ethiopic Supplement */
	{ 0x2D80, 0x2DDF, "Ethiopic" },	  /* Ethiopic Extended */
	{ 0xAB01, 0xAB2F, "Ethiopic" },	  /* Ethiopic Extended-A */
	{ 0x1E7E0, 0x1E7FF, "Ethiopic" }, /* Ethiopic Extended-B */
	{ 0x2D30, 0x2D7F, "Tifinagh" },
	{ 0x07C0, 0x07FF, "Nko" },
};

const char *unicodeScriptName(uint32_t cp) {
	size_t count = sizeof(known_scripts) / sizeof(known_scripts[0]);
	for (size_t i = 0; i < count; i++) {
		if (cp >= known_scripts[i].start &&
		    cp <= known_scripts[i].end) {
			return known_scripts[i].name;
		}
	}
	return NULL;
}

/* Return the Unicode name for non-printing characters.
 * Returns NULL if the character is printable. */
const char *unicodeCharName(uint32_t cp) {
	/* C0 control characters */
	if (cp == 0x00)
		return "NULL";
	if (cp == 0x09)
		return "CHARACTER TABULATION";
	if (cp == 0x0D)
		return "CARRIAGE RETURN";
	if (cp == 0x7F)
		return "DELETE";
	if (cp < 0x20) {
		static char ctrl_name[16];
		snprintf(ctrl_name, sizeof(ctrl_name), "CONTROL-%04X", cp);
		return ctrl_name;
	}

	/* Unicode space characters */
	if (cp == 0x0020)
		return "SPACE";
	if (cp == 0x00A0)
		return "NO-BREAK SPACE";
	if (cp == 0x1680)
		return "OGHAM SPACE MARK";
	if (cp == 0x2000)
		return "EN QUAD";
	if (cp == 0x2001)
		return "EM QUAD";
	if (cp == 0x2002)
		return "EN SPACE";
	if (cp == 0x2003)
		return "EM SPACE";
	if (cp == 0x2004)
		return "THREE-PER-EM SPACE";
	if (cp == 0x2005)
		return "FOUR-PER-EM SPACE";
	if (cp == 0x2006)
		return "SIX-PER-EM SPACE";
	if (cp == 0x2007)
		return "FIGURE SPACE";
	if (cp == 0x2008)
		return "PUNCTUATION SPACE";
	if (cp == 0x2009)
		return "THIN SPACE";
	if (cp == 0x200A)
		return "HAIR SPACE";
	if (cp == 0x202F)
		return "NARROW NO-BREAK SPACE";
	if (cp == 0x205F)
		return "MEDIUM MATHEMATICAL SPACE";
	if (cp == 0x3000)
		return "IDEOGRAPHIC SPACE";

	/* Zero-width characters */
	if (cp == 0x200B)
		return "ZERO WIDTH SPACE";
	if (cp == 0x200C)
		return "ZERO WIDTH NON-JOINER";
	if (cp == 0x200D)
		return "ZERO WIDTH JOINER";
	if (cp == 0xFEFF)
		return "ZERO WIDTH NO-BREAK SPACE";

	return NULL;
}

/* Decode the UTF-8 character at str[idx] and return its Unicode codepoint. */
uint32_t utf8Decode(const uint8_t *str, int idx) {
	/* Each continuation byte is verified before the byte after it
	 * is read.  Rows are NUL-terminated at chars[size], and NUL is
	 * never a continuation byte, so decoding stops at the
	 * terminator for truncated sequences (which can reach a buffer
	 * via byte-column rectangle operations on multibyte text)
	 * instead of reading past the allocation.  
	 */
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

/* Encode a Unicode codepoint into UTF-8 bytes at out[].
 * Returns the number of bytes written, or 0 if the codepoint
 * is invalid (e.g., exceeds U+10FFFF or is a surrogate).
 */
int utf8Encode(uint32_t cp, uint8_t *out) {
	if (cp < 0x80) {
		out[0] = cp;
		return 1;
	} else if (cp < 0x800) {
		out[0] = 0xC0 | (cp >> 6);
		out[1] = 0x80 | (cp & 0x3F);
		return 2;
	} else if (cp < 0x10000) {
		/* UTF-16 surrogates are invalid Unicode */
		if (cp >= 0xD800 && cp <= 0xDFFF)
			return 0;
		out[0] = 0xE0 | (cp >> 12);
		out[1] = 0x80 | ((cp >> 6) & 0x3F);
		out[2] = 0x80 | (cp & 0x3F);
		return 3;
	} else if (cp < 0x110000) {
		out[0] = 0xF0 | (cp >> 18);
		out[1] = 0x80 | ((cp >> 12) & 0x3F);
		out[2] = 0x80 | ((cp >> 6) & 0x3F);
		out[3] = 0x80 | (cp & 0x3F);
		return 4;
	}
	return 0;
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

/* 行首禁则 are the characters forbidden at the start of a wrapped line.
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
 * ANGKHANKHU) and ๛ (U+0E5B KHOMUT) for classical texts.  
 */
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

/* THE single display-width rule (issue #117 R1).
 *
 * Columns occupied by the character starting at str[idx] when it is
 * drawn at display column x.  x matters only for '\t', whose width is
 * the distance to the next tab stop.  *nbytes (may be NULL) receives
 * the character's encoded length.
 *
 * The rule, in full:
 *   '\t'                     -> EMIL_TAB_STOP - (x % EMIL_TAB_STOP)
 *   control incl. NUL, DEL   -> 2   (displayed as ^X / ^@ / ^?)
 *   other ASCII              -> 1
 *   multibyte                -> wcwidth via charInStringWidth
 *                               (negative -> 1)
 *
 * Every walk that accumulates display columns over row or message
 * bytes must get its per-character widths from here.  Before this
 * function existed the rule was open-coded nine times, and copies
 * that had to agree exactly — wordWrapBreak deciding where a sub-line
 * ends versus the render loop drawing it; wordWrapBreak versus
 * displayColumnToByteOffset navigating within the sub-line it defined
 * — were kept in agreement only by inspection, which had already
 * failed on the NUL byte (DEF-5: one copy said 1 column, the other
 * three said 2).
 *
 * NUL is deliberately 2 here, siding with the majority and with
 * charInStringWidth: a NUL cannot reach a buffer (load rejects it,
 * §3.21.1) but the rule must still answer, and answering differently
 * per caller is exactly the defect class this function removes. */
int charAdvance(const uint8_t *str, int idx, int x, int *nbytes) {
	uint8_t ch = str[idx];
	int width;

	if (ch == '\t') {
		width = EMIL_TAB_STOP - (x % EMIL_TAB_STOP);
	} else if (ch < 0x20 || ch == 0x7f) {
		width = 2;
	} else if (ch < 0x80) {
		width = 1;
	} else {
		width = charInStringWidth(str, idx);
		if (width < 0)
			width = 1;
	}

	if (nbytes) {
		int nb = utf8_nBytes(ch);
		*nbytes = nb > 0 ? nb : 1;
	}
	return width;
}

/* Byte offset of the first character in str[from..from+len) that would
 * not fit within `cols` display columns, walking whole characters only.
 * *used (may be NULL) receives the columns actually consumed, which can
 * be short of `cols` when a wide character straddles the boundary.
 *
 * Column accounting starts at 0 relative to `from` (tabs expand
 * against that origin).  Honest about zero progress: if the first
 * character alone exceeds `cols`, the return is `from` and *used is 0;
 * a caller that must advance regardless (the minibuffer fill) forces
 * one character through itself. */
int utf8ColsToBytes(const uint8_t *str, int from, int len, int cols,
		    int *used) {
	int idx = from;
	int x = 0;

	while (idx < from + len) {
		int nb;
		int w = charAdvance(str, idx, x, &nb);
		if (x + w > cols)
			break;
		x += w;
		idx += nb;
	}
	if (used)
		*used = x;
	return idx;
}

/* Total display columns of str[0..len), under THE rule.
 *
 * stringWidth() answers the same question for a NUL-terminated string
 * but prices a tab at 2 (charInStringWidth's control-character rule)
 * rather than by tab stop.  Anything measuring text that will be laid
 * out on screen wants this one. */
int utf8WidthN(const uint8_t *str, int len) {
	int total = 0;
	for (int i = 0; i < len;) {
		int nb;
		total += charAdvance(str, i, total, &nb);
		i += nb;
	}
	return total;
}

/* Byte offset of the first character to KEEP so that the remainder of
 * str[0..len) fits within `budget` display columns, dropping whole
 * characters from the left.  Returns len if nothing fits.
 *
 * This is left-truncation's walk, shared by leftTruncate() (which
 * allocates a "..." + tail string) and statusLeft() (which formats
 * one into a fixed buffer).  They are the two places that truncate a
 * display_name, and the #117 report found them disagreeing — one
 * measuring bytes, the other columns.  Sharing the width rule was not
 * enough to stop that recurring: the walk itself has to be one
 * function, or the next divergence is a copy-edit away. */
int utf8DropToFit(const uint8_t *str, int len, int budget) {
	int total = utf8WidthN(str, len);
	int dropped = 0;
	int i = 0;

	while (i < len && total - dropped > budget) {
		int nb;
		dropped += charAdvance(str, i, dropped, &nb);
		i += nb;
	}
	return i;
}

int nextScreenX(uint8_t *str, int *idx, int screen_x) {
	/* Wrapper over charAdvance so the historical interface — and
	 * test_wcwidth.c through it — keeps testing the same rule.
	 * Advances *idx to the character's LAST byte; the caller's
	 * loop increment steps onto the next character. */
	int nbytes;
	int width = charAdvance(str, *idx, screen_x, &nbytes);
	*idx += nbytes - 1;
	return screen_x + width;
}

/* Move cx to the nearest UTF-8 character boundary within a row.
 *
 * A cursor byte offset is legal only at the start of a character or at
 * the very end of the row.  Any code that carries a byte offset from
 * one row to another -- vertical motion, scrolling, restoring a saved
 * position -- can land inside a multibyte character and must snap.
 *
 * dir > 0 snaps forward, which is what vertical motion does: moving up
 * or down a line and finding yourself inside a character puts you at
 * the next one.  dir <= 0 snaps backward, for callers that are moving
 * left and must not jump the cursor forward past the character they
 * were headed for.
 *
 * cx == size is the end-of-line position and is already a boundary; it
 * is returned unchanged rather than snapped, and the early return also
 * keeps the backward scan from reading chars[size]. */
int utf8_snapToBoundary(const uint8_t *chars, int size, int cx, int dir) {
	if (cx <= 0)
		return 0;
	if (cx >= size)
		return size;

	if (dir > 0) {
		while (cx < size && utf8_isCont(chars[cx]))
			cx++;
	} else {
		while (cx > 0 && utf8_isCont(chars[cx]))
			cx--;
	}
	return cx;
}
