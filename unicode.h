/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#ifndef EMIL_UNICODE_H
#define EMIL_UNICODE_H 1

#include <stdint.h>

const char *unicodeScriptName(uint32_t cp);
const char *unicodeCharName(uint32_t cp);

int stringWidth(const uint8_t *str);

int charInStringWidth(const uint8_t *str, int idx);

int utf8_is2Char(uint8_t ch);

int utf8_is3Char(uint8_t ch);

int utf8_is4Char(uint8_t ch);

int utf8_nBytes(uint8_t ch);

int utf8_isCont(uint8_t ch);

uint32_t utf8Decode(const uint8_t *str, int idx);

int utf8Encode(uint32_t cp, uint8_t *out);

/* Codepoint classifiers */
int isCJKChar(uint32_t cp);
int isLineStartForbidden(uint32_t cp);
int isSEAsianSentenceTerminator(uint32_t cp);
int isWordSeparatorCP(uint32_t cp);
int isPreposedVowel(uint32_t cp);
int isCJKSentenceTerminator(uint32_t cp);
int isIndicSentenceTerminator(uint32_t cp);

int utf8_validate(const uint8_t *buf, int len);

/* The single display-width rule; see the comment at the definition.
 * All per-character column accounting routes through charAdvance. */
int charAdvance(const uint8_t *str, int idx, int x, int *nbytes);
int utf8ColsToBytes(const uint8_t *str, int from, int len, int cols, int *used);
int utf8WidthN(const uint8_t *str, int len);
int utf8DropToFit(const uint8_t *str, int len, int budget);

int nextScreenX(uint8_t *str, int *idx, int screen_x);

int utf8_snapToBoundary(const uint8_t *chars, int size, int cx, int dir);

/* Select a UTF-8 locale for LC_CTYPE if the platform has one, and say
 * whether it worked.  Idempotent and cached; safe to call from anywhere.
 *
 * Emil decodes UTF-8 itself and never uses the libc multibyte
 * functions, so the locale matters for exactly one thing: wcwidth().
 * Without a UTF-8 locale wcwidth reports -1 for every non-ASCII
 * codepoint -- correctly, since they have no defined width in the C
 * locale -- and charAdvance maps that to one column.
 *
 * Returns 1 when wide characters will measure 2 columns, 0 when the
 * platform offers only the C locale and everything non-ASCII will
 * measure 1.  Genode is the second case: its libc build filters out
 * setlocale.c, setrunelocale.c and every encoding module, so there is
 * no UTF-8 locale to select and no configuration that adds one.
 *
 * The wcwidth probe is the check, not setlocale's return value:
 * Genode's stub returns "C" rather than NULL for a locale it did not
 * set, so trusting the return would conclude success wrongly. */
int selectUtf8Locale(void);

#endif /* EMIL_UNICODE_H */
