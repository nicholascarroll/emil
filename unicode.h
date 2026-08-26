/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
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

int nextScreenX(uint8_t *str, int *idx, int screen_x);

int utf8_snapToBoundary(const uint8_t *chars, int size, int cx, int dir);
