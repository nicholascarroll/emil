#include <stdint.h>

int unicodeTest(void);

int stringWidth(const uint8_t *str);

int charInStringWidth(const uint8_t *str, int idx);

int utf8_is2Char(uint8_t ch);

int utf8_is3Char(uint8_t ch);

int utf8_is4Char(uint8_t ch);

int utf8_nBytes(uint8_t ch);

int utf8_isCont(uint8_t ch);

uint32_t utf8Decode(const uint8_t *str, int idx);

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
