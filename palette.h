#ifndef EMIL_PALETTE_H
#define EMIL_PALETTE_H

#include <stdint.h>
#include <stdbool.h>

/* Sentinel codepoint for category break entries.
   Never a real Unicode codepoint; invisible to palette_wcwidth. */
#define PALETTE_BREAK 0x00

typedef struct {
	uint32_t codepoint; /* PALETTE_BREAK for category separators */
	unsigned char utf8[5];
	int utf8_len;
	int width; /* Authoritative wcwidth value */
	bool default_sel;
} PaletteEntry;

extern const PaletteEntry palette[];
extern const int palette_size;

/* Use to intercept system wcwidth: returns our known width if codepoint is in
 * the palette, otherwise returns -1, in which case, fall to system wcwidth. */
int palette_wcwidth(uint32_t codepoint);

/* Open the palette popup; on Enter insert the selected symbol into
 * the buffer that was active at invocation time. */
void expandPalette(void);

#endif
