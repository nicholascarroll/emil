#ifndef EMIL_PALETTE_H
#define EMIL_PALETTE_H

#include <stdint.h>
#include <stdbool.h>

/* Sentinel codepoint for category break entries.
   Never a real Unicode codepoint. */
#define PALETTE_BREAK 0x00

typedef struct {
	uint32_t codepoint; /* PALETTE_BREAK for category separators */
	unsigned char utf8[5];
	int utf8_len;
	bool default_sel;
} PaletteEntry;

extern const PaletteEntry palette[];
extern const int palette_size;

/* Open the palette popup; on Enter insert the selected symbol into
 * the buffer that was active at invocation time. */
void expandPalette(void);

#endif
