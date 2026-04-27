#include <stdint.h>

int unicodeTest(void);

int stringWidth(const uint8_t *str);

int charInStringWidth(const uint8_t *str, int idx);

int utf8_is2Char(uint8_t ch);

int utf8_is3Char(uint8_t ch);

int utf8_is4Char(uint8_t ch);

int utf8_nBytes(uint8_t ch);

int utf8_isCont(uint8_t ch);

int utf8ToUCS(const uint8_t *str, int idx);

int utf8_validate(const uint8_t *buf, int len);

int nextScreenX(uint8_t *str, int *idx, int screen_x);

#ifdef EMIL_DEBUG_WCWIDTH
/* Runtime toggle between bundled (Unicode 17.0) and system wcwidth.
 * To remove: delete widechar_width_c.h and all EMIL_DEBUG_WCWIDTH blocks. */
void unicode_toggle_wcwidth(void);
const char *unicode_wcwidth_source(void);
#endif
