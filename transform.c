#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "transform.h"
#include "unicode.h"
#include "emil.h"
#include "message.h"
#include "buffer.h"
#include "undo.h"
#include "display.h"
#include "unused.h"
#include "region.h"
#include "util.h"

#define MKOUTPUT(in, l, o)          \
	int l = strlen((char *)in); \
	uint8_t *o = xmalloc(l + 1)

/* ---- Latin Extended case mapping (U+00C0–U+017F) ---- */

/* Decode a 2-byte UTF-8 sequence to a codepoint.
 * Returns the codepoint, or -1 if not a valid 2-byte sequence. */
static int decode2(uint8_t b0, uint8_t b1) {
	if ((b0 & 0xE0) != 0xC0 || (b1 & 0xC0) != 0x80)
		return -1;
	return ((b0 & 0x1F) << 6) | (b1 & 0x3F);
}

/* Encode a codepoint (must be U+0080–U+07FF) into 2 UTF-8 bytes. */
static void encode2(int cp, uint8_t *b0, uint8_t *b1) {
	*b0 = 0xC0 | ((cp >> 6) & 0x1F);
	*b1 = 0x80 | (cp & 0x3F);
}

/* Return the uppercase form of a Latin Unicode codepoint in the range
 * U+00C0–U+017F.  Returns the codepoint unchanged if no mapping exists.
 * Input/output are Unicode codepoints, not UTF-8 bytes. */
static int latin_toupper(int cp) {
	/* Latin-1 Supplement: U+00E0–U+00FE lowercase → U+00C0–U+00DE
	 * Skip U+00F7 (÷) which is not a letter. */
	if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7)
		return cp - 0x20;

	/* Latin Extended-A: even=upper, odd=lower for most pairs */
	if (cp >= 0x0101 && cp <= 0x012F && (cp & 1))
		return cp - 1;
	if (cp >= 0x0133 && cp <= 0x0177 && (cp & 1))
		return cp - 1;

	/* U+00FF ÿ → U+0178 Ÿ */
	if (cp == 0xFF)
		return 0x0178;

	return cp;
}

/* Return the lowercase form of a Latin Unicode codepoint in the range
 * U+00C0–U+017F.  Returns the codepoint unchanged if no mapping exists.
 * Input/output are Unicode codepoints, not UTF-8 bytes. */
static int latin_tolower(int cp) {
	/* Latin-1 Supplement: U+00C0–U+00DE uppercase → U+00E0–U+00FE
	 * Skip U+00D7 (×) which is not a letter. */
	if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7)
		return cp + 0x20;

	/* Latin Extended-A: even=upper, odd=lower for most pairs */
	if (cp >= 0x0100 && cp <= 0x012E && !(cp & 1))
		return cp + 1;
	if (cp >= 0x0132 && cp <= 0x0176 && !(cp & 1))
		return cp + 1;

	/* U+0178 Ÿ → U+00FF ÿ */
	if (cp == 0x0178)
		return 0xFF;

	return cp;
}

/* Is this codepoint a Latin letter (has distinct upper/lower forms)? */
static int latin_isLetter(int cp) {
	return latin_toupper(cp) != latin_tolower(cp);
}

uint8_t *transformerUpcase(uint8_t *input) {
	MKOUTPUT(input, len, output);

	for (int i = 0; i <= len; i++) {
		uint8_t c = input[i];
		if ('a' <= c && c <= 'z') {
			output[i] = c & 0x5f;
		} else if (utf8_is2Char(c) && i + 1 <= len) {
			int cp = decode2(c, input[i + 1]);
			if (cp >= 0) {
				int mapped = latin_toupper(cp);
				encode2(mapped, &output[i], &output[i + 1]);
			} else {
				output[i] = c;
				output[i + 1] = input[i + 1];
			}
			i++; /* skip second byte */
		} else {
			output[i] = c;
		}
	}

	return output;
}

uint8_t *transformerDowncase(uint8_t *input) {
	MKOUTPUT(input, len, output);

	for (int i = 0; i <= len; i++) {
		uint8_t c = input[i];
		if ('A' <= c && c <= 'Z') {
			output[i] = c | 0x60;
		} else if (utf8_is2Char(c) && i + 1 <= len) {
			int cp = decode2(c, input[i + 1]);
			if (cp >= 0) {
				int mapped = latin_tolower(cp);
				encode2(mapped, &output[i], &output[i + 1]);
			} else {
				output[i] = c;
				output[i + 1] = input[i + 1];
			}
			i++; /* skip second byte */
		} else {
			output[i] = c;
		}
	}

	return output;
}

uint8_t *transformerCapitalCase(uint8_t *input) {
	MKOUTPUT(input, len, output);

	int first = 1;

	for (int i = 0; i <= len; i++) {
		uint8_t c = input[i];
		if (utf8_is2Char(c) && i + 1 <= len) {
			int cp = decode2(c, input[i + 1]);
			if (cp >= 0 && latin_isLetter(cp)) {
				int mapped;
				if (first) {
					mapped = latin_toupper(cp);
					first = 0;
				} else {
					mapped = latin_tolower(cp);
				}
				encode2(mapped, &output[i], &output[i + 1]);
			} else {
				output[i] = c;
				output[i + 1] = input[i + 1];
			}
			i++; /* skip second byte */
		} else if ((('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z')) &&
			   first) {
			first = 0;
			output[i] = c & 0x5f;
		} else if ('A' <= c && c <= 'Z') {
			output[i] = c | 0x60;
		} else if (isWordBoundary(c)) {
			first = 1;
			output[i] = c;
		} else {
			output[i] = c;
		}
	}

	return output;
}

uint8_t *transformerTransposeChars(uint8_t *input) {
	MKOUTPUT(input, len, output);

	int endFirst = utf8_nBytes(input[0]);

	memcpy(output, input + endFirst, len - endFirst);
	memcpy(output + (len - endFirst), input, endFirst);

	output[len] = 0;

	return output;
}

uint8_t *transformerTransposeWords(uint8_t *input) {
	MKOUTPUT(input, len, output);

	int endFirst = 0, startSecond = 0;
	int which = 0;
	for (int i = 0; i <= len; i++) {
		if (!which) {
			if (isWordBoundary(input[i])) {
				which++;
				endFirst = i;
			}
		} else {
			if (!isWordBoundary(input[i])) {
				startSecond = i;
				break;
			}
		}
	}
	int offset = 0;
	memcpy(output, input + startSecond, len - startSecond);
	offset += len - startSecond;
	memcpy(output + offset, input + endFirst, startSecond - endFirst);
	offset += startSecond - endFirst;
	memcpy(output + offset, input, endFirst);

	output[len] = 0;

	return output;
}

void editorCapitalizeRegion(struct editorConfig *ed, struct editorBuffer *buf) {
	editorTransformRegion(ed, buf, transformerCapitalCase);
}
