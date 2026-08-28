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

#endif /* EMIL_UNICODE_H */
