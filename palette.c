/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include "palette.h"
#include "buffer.h"
#include "display.h"
#include "edit.h"
#include "emil.h"
#include "keymap.h"
#include "motion.h"
#include "terminal.h"
#include "unicode.h"
#include "util.h"
#include "window.h"
#include <string.h>

const PaletteEntry palette[] = {
	/* Punctuation and Misc */
	{ 0x2014, "EM DASH" },					  // —
	{ 0x2713, "CHECK MARK" },				  // ✓
	{ 0x2717, "BALLOT X" },					  // ✗
	{ 0x2013, "EN DASH" },					  // –
	{ 0x2026, "HORIZONTAL ELLIPSIS" },			  // …
	{ 0x201C, "LEFT DOUBLE QUOTATION MARK" },		  // “
	{ 0x201D, "RIGHT DOUBLE QUOTATION MARK" },		  // ”
	{ 0x2018, "LEFT SINGLE QUOTATION MARK" },		  // ‘
	{ 0x2019, "RIGHT SINGLE QUOTATION MARK" },		  // ’
	{ 0x00AB, "LEFT-POINTING DOUBLE ANGLE QUOTATION MARK" },  // «
	{ 0x00BB, "RIGHT-POINTING DOUBLE ANGLE QUOTATION MARK" }, // »
	{ 0x2039, "SINGLE LEFT-POINTING ANGLE QUOTATION MARK" },  // ‹
	{ 0x203A, "SINGLE RIGHT-POINTING ANGLE QUOTATION MARK" }, // ›
	{ 0x2010, "HYPHEN" },					  // ‐
	{ 0x00B7, "MIDDLE DOT" },				  // ·
	/* Publishing */
	{ 0x00A7, "SECTION SIGN" }, // §
	{ 0x2032, "PRIME" },	    // ′
	{ 0x2033, "DOUBLE PRIME" }, // ″
	/* Legal */
	{ 0x00A9, "COPYRIGHT SIGN" },  // ©
	{ 0x00AE, "REGISTERED SIGN" }, // ®
	{ 0x2122, "TRADE MARK SIGN" }, // ™
	{ 0x00B6, "PILCROW SIGN" },    // ¶
	{ 0x2020, "DAGGER" },	       // †
	{ 0x2021, "DOUBLE DAGGER" },   // ‡
	{0x00B0,  " DEGREE SIGN"  },   // °
	/* Currency */
	{ 0x00A2, "CENT SIGN" },		 // ¢
	{ 0x00A3, "POUND SIGN" },		 // £
	{ 0x00A5, "YEN SIGN" },			 // ¥
	{ 0x20AC, "EURO SIGN" },		 // €
	{ 0x20A9, "WON SIGN" },			 // ₩
	{ 0x20B9, "INDIAN RUPEE SIGN" },	 // ₹
	{ 0x20BD, "RUBLE SIGN" },		 // ₽
	{ 0x0E3F, "THAI CURRENCY SYMBOL BAHT" }, // ฿
	{ 0x20BA, "TURKISH LIRA SIGN" },	 // ₺
	{ 0x20B1, "PESO SIGN" },		 // ₱
	{ 0x20AA, "NEW SHEQEL SIGN" },		 // ₪
	{ 0x20AB, "DONG SIGN" },		 // ₫
	{ 0x20AE, "TUGRIK SIGN" },		 // ₮
	{ 0x20B4, "HRYVNIA SIGN" },		 // ₴
	{ 0x20BF, "BITCOIN SIGN" },		 // ₿

	/* Emoji */
	{ PALETTE_BREAK, NULL },
	{ 0x1F44D, "THUMBS UP SIGN" },					// 👍
	{ 0x1F44E, "THUMBS DOWN SIGN" },				// 👎
	{ 0x1F440, "EYES" },						// 👀
	{ 0x1F44B, "WAVING HAND SIGN" },				// 👋
	{ 0x1F44C, "OK HAND SIGN" },					// 👌
	{ 0x1F601, "GRINNING FACE WITH SMILING EYES" },			// 😁
	{ 0x1F600, "GRINNING FACE" },					// 😀
	{ 0x1F602, "FACE WITH TEARS OF JOY" },				// 😂
	{ 0x1F60A, "SMILING FACE WITH SMILING EYES" },			// 😊
	{ 0x1F609, "WINKING FACE" },					// 😉
	{ 0x1F60D, "SMILING FACE WITH HEART-SHAPED EYES" },		// 😍
	{ 0x1F60E, "SMILING FACE WITH SUNGLASSES" },			// 😎
	{ 0x1F622, "CRYING FACE" },					// 😢
	{ 0x1F62D, "LOUDLY CRYING FACE" },				// 😭
	{ 0x1F630, "FACE WITH OPEN MOUTH AND COLD SWEAT" },		// 😰
	{ 0x1F633, "FLUSHED FACE" },					// 😳
	{ 0x1F923, "ROLLING ON THE FLOOR LAUGHING" },			// 🤣
	{ 0x1F973, "FACE WITH PARTY HORN AND PARTY HAT" },		// 🥳
	{ 0x1F977, "NINJA" },						// 🥷
	{ 0x1F97A, "FACE WITH PLEADING EYES" },				// 🥺
	{ 0x1F382, "SHORTCAKE" },					// 🎂
	{ 0x1F389, "PARTY POPPER" },					// 🎉
	{ 0x1F4A1, "ELECTRIC LIGHT BULB" },				// 💡
	{ 0x1F525, "FIRE" },						// 🔥
	{ 0x1F680, "ROCKET" },						// 🚀
	{ 0x1F517, "LINK SYMBOL" },					// 🔗
	{ 0x1F48B, "KISS MARK" },					// 💋
	{ 0x1F495, "TWO HEARTS" },					// 💕
	{ 0x1F496, "SPARKLING HEART" },					// 💖
	{ 0x1F970, "SMILING FACE WITH SMILING EYES AND THREE HEARTS" }, // 🥰
	{ 0x1F498, "HEART WITH ARROW" },				// 💘
	{ 0x1F494, "BROKEN HEART" },					// 💔
	{ 0x1F337, "TULIP" },						// 🌷
	{ 0x1F339, "ROSE" },						// 🌹
	{ 0x1F33B, "SUNFLOWER" },					// 🌻
	{ 0x1F33C, "BLOSSOM" },						// 🌼
	{ 0x1F408, "CAT" },						// 🐈
	{ 0x1F415, "DOG" },						// 🐕
	{ 0x2705, "WHITE HEAVY CHECK MARK" },				// ✅
	{ 0x274C, "CROSS MARK" },					// ❌
	{ 0x2728, "SPARKLES" },						// ✨
	{ 0x26A1, "HIGH VOLTAGE SIGN" },				// ⚡
	{ 0x2B50, "WHITE MEDIUM STAR" },				// ⭐
	{ 0x270B, "RAISED HAND" },					// ✋
	{ 0x1FAE1, "SALUTING FACE" },					// 🫡
	{ 0x1F60F, "SMIRKING FACE" },					// 😏
	{ 0x1F610, "NEUTRAL FACE" },					// 😐
	{ 0x1F612, "UNAMUSED FACE" },					// 😒
	{ 0x1F618, "FACE THROWING A KISS" },				// 😘
	{ 0x1F61C, "WINKING FACE WITH STUCK-OUT TONGUE" },		// 😜
	{ 0x1F92A, "GRINNING FACE WITH ONE LARGE AND ONE SMALL EYE" },	// 🤪
	{ 0x1F61F, "WORRIED FACE" },					// 😟
	{ 0x1F620, "ANGRY FACE" },					// 😠
	{ 0x1F621, "POUTING FACE" },					// 😡
	{ 0x1F62C, "GRIMACING FACE" },					// 😬
	{ 0x1F625, "DISAPPOINTED BUT RELIEVED FACE" },			// 😥
	{ 0x1F641, "SLIGHTLY FROWNING FACE" },				// 🙁
	{ 0x1F642, "SLIGHTLY SMILING FACE" },				// 🙂
	{ 0x1F914, "THINKING FACE" },					// 🤔
	{ 0x1F925, "LYING FACE" },					// 🤥
	{ 0x1F928, "FACE WITH ONE EYEBROW RAISED" },			// 🤨

	/* Greek Letters */
	{ PALETTE_BREAK, NULL },
	{ 0x03B1, "GREEK SMALL LETTER ALPHA" },	  // α
	{ 0x03B2, "GREEK SMALL LETTER BETA" },	  // β
	{ 0x03B3, "GREEK SMALL LETTER GAMMA" },	  // γ
	{ 0x03B4, "GREEK SMALL LETTER DELTA" },	  // δ
	{ 0x03B5, "GREEK SMALL LETTER EPSILON" }, // ε
	{ 0x03B6, "GREEK SMALL LETTER ZETA" },	  // ζ
	{ 0x03B7, "GREEK SMALL LETTER ETA" },	  // η
	{ 0x03B8, "GREEK SMALL LETTER THETA" },	  // θ
	{ 0x03B9, "GREEK SMALL LETTER IOTA" },	  // ι
	{ 0x03BA, "GREEK SMALL LETTER KAPPA" },	  // κ
	{ 0x03BB, "GREEK SMALL LETTER LAMDA" },	  // λ
	{ 0x03BC, "GREEK SMALL LETTER MU" },	  // μ
	{ 0x03BD, "GREEK SMALL LETTER NU" },	  // ν
	{ 0x03BE, "GREEK SMALL LETTER XI" },	  // ξ
	{ 0x03BF, "GREEK SMALL LETTER OMICRON" }, // ο
	{ 0x03C0, "GREEK SMALL LETTER PI" },	  // π
	{ 0x03C1, "GREEK SMALL LETTER RHO" },	  // ρ
	{ 0x03C3, "GREEK SMALL LETTER SIGMA" },	  // σ
	{ 0x03C4, "GREEK SMALL LETTER TAU" },	  // τ
	{ 0x03C5, "GREEK SMALL LETTER UPSILON" }, // υ
	{ 0x03C6, "GREEK SMALL LETTER PHI" },	  // φ
	{ 0x03C7, "GREEK SMALL LETTER CHI" },	  // χ
	{ 0x03C8, "GREEK SMALL LETTER PSI" },	  // ψ
	{ 0x03C9, "GREEK SMALL LETTER OMEGA" },	  // ω
	{ 0x0393, "GREEK CAPITAL LETTER GAMMA" }, // Γ
	{ 0x0394, "GREEK CAPITAL LETTER DELTA" }, // Δ
	{ 0x0398, "GREEK CAPITAL LETTER THETA" }, // Θ
	{ 0x039B, "GREEK CAPITAL LETTER LAMDA" }, // Λ
	{ 0x039E, "GREEK CAPITAL LETTER XI" },	  // Ξ
	{ 0x03A0, "GREEK CAPITAL LETTER PI" },	  // Π
	{ 0x03A3, "GREEK CAPITAL LETTER SIGMA" }, // Σ
	{ 0x03A6, "GREEK CAPITAL LETTER PHI" },	  // Φ
	{ 0x03A8, "GREEK CAPITAL LETTER PSI" },	  // Ψ
	{ 0x03A9, "GREEK CAPITAL LETTER OMEGA" }, // Ω
	/* Box Drawing - Light & Heavy */
	{ PALETTE_BREAK, NULL },
	{ 0x2500, "BOX DRAWINGS LIGHT HORIZONTAL" },		  // ─
	{ 0x2502, "BOX DRAWINGS LIGHT VERTICAL" },		  // │
	{ 0x250C, "BOX DRAWINGS LIGHT DOWN AND RIGHT" },	  // ┌
	{ 0x2510, "BOX DRAWINGS LIGHT DOWN AND LEFT" },		  // ┐
	{ 0x2514, "BOX DRAWINGS LIGHT UP AND RIGHT" },		  // └
	{ 0x2518, "BOX DRAWINGS LIGHT UP AND LEFT" },		  // ┘
	{ 0x251C, "BOX DRAWINGS LIGHT VERTICAL AND RIGHT" },	  // ├
	{ 0x2524, "BOX DRAWINGS LIGHT VERTICAL AND LEFT" },	  // ┤
	{ 0x252C, "BOX DRAWINGS LIGHT DOWN AND HORIZONTAL" },	  // ┬
	{ 0x2534, "BOX DRAWINGS LIGHT UP AND HORIZONTAL" },	  // ┴
	{ 0x253C, "BOX DRAWINGS LIGHT VERTICAL AND HORIZONTAL" }, // ┼
	{ 0x2501, "BOX DRAWINGS HEAVY HORIZONTAL" },		  // ━
	{ 0x2503, "BOX DRAWINGS HEAVY VERTICAL" },		  // ┃
	{ 0x250F, "BOX DRAWINGS HEAVY DOWN AND RIGHT" },	  // ┏
	{ 0x251B, "BOX DRAWINGS HEAVY UP AND LEFT" },		  // ┛
	/* Box Drawing - Double */
	{ 0x2551, "BOX DRAWINGS DOUBLE VERTICAL" },		   // ║
	{ 0x2550, "BOX DRAWINGS DOUBLE HORIZONTAL" },		   // ═
	{ 0x2554, "BOX DRAWINGS DOUBLE DOWN AND RIGHT" },	   // ╔
	{ 0x2557, "BOX DRAWINGS DOUBLE DOWN AND LEFT" },	   // ╗
	{ 0x255A, "BOX DRAWINGS DOUBLE UP AND RIGHT" },		   // ╚
	{ 0x255D, "BOX DRAWINGS DOUBLE UP AND LEFT" },		   // ╝
	{ 0x2560, "BOX DRAWINGS DOUBLE VERTICAL AND RIGHT" },	   // ╠
	{ 0x2563, "BOX DRAWINGS DOUBLE VERTICAL AND LEFT" },	   // ╣
	{ 0x2566, "BOX DRAWINGS DOUBLE DOWN AND HORIZONTAL" },	   // ╦
	{ 0x2569, "BOX DRAWINGS DOUBLE UP AND HORIZONTAL" },	   // ╩
	{ 0x256C, "BOX DRAWINGS DOUBLE VERTICAL AND HORIZONTAL" }, // ╬
	/* Terminal/CLI Blocks & Shading */
	{ PALETTE_BREAK, NULL },
	{ 0x2588, "FULL BLOCK" },		     // █
	{ 0x2589, "LEFT SEVEN EIGHTHS BLOCK" },	     // ▉
	{ 0x258A, "LEFT THREE QUARTERS BLOCK" },     // ▊
	{ 0x258B, "LEFT FIVE EIGHTHS BLOCK" },	     // ▋
	{ 0x258C, "LEFT HALF BLOCK" },		     // ▌
	{ 0x258D, "LEFT THREE EIGHTHS BLOCK" },	     // ▍
	{ 0x258E, "LEFT ONE QUARTER BLOCK" },	     // ▎
	{ 0x258F, "LEFT ONE EIGHTH BLOCK" },	     // ▏
	{ 0x2581, "LOWER ONE EIGHTH BLOCK" },	     // ▁
	{ 0x2582, "LOWER ONE QUARTER BLOCK" },	     // ▂
	{ 0x2583, "LOWER THREE EIGHTHS BLOCK" },     // ▃
	{ 0x2584, "LOWER HALF BLOCK" },		     // ▄
	{ 0x2585, "LOWER FIVE EIGHTHS BLOCK" },	     // ▅
	{ 0x2586, "LOWER THREE QUARTERS BLOCK" },    // ▆
	{ 0x2587, "LOWER SEVEN EIGHTHS BLOCK" },     // ▇
	{ 0x2591, "LIGHT SHADE" },		     // ░
	{ 0x2592, "MEDIUM SHADE" },		     // ▒
	{ 0x2593, "DARK SHADE" },		     // ▓
	{ 0x25B2, "BLACK UP-POINTING TRIANGLE" },    // ▲
	{ 0x25BC, "BLACK DOWN-POINTING TRIANGLE" },  // ▼
	{ 0x25B6, "BLACK RIGHT-POINTING TRIANGLE" }, // ▶
	{ 0x25C0, "BLACK LEFT-POINTING TRIANGLE" },  // ◀
};
const int palette_size = sizeof(palette) / sizeof(palette[0]);

/* ------------------------------------------------------------------ */
/* Palette popup                                                       */
/* ------------------------------------------------------------------ */
#define PALETTE_BUF_NAME "Palette"

/* Populate the palette buffer.  Mirrors the dump_palette utility:
concatenate every entry's utf8[] with a trailing space.  The
PALETTE_BREAK entries contain '\n', producing line breaks. */
static void populatePaletteBuffer(struct buffer *buf) {
	bufferResetRows(buf);
	buf->read_only = 0;
	uint8_t flat[4096];
	int len = 0;
	for (int i = 0; i < palette_size; i++) {
		if (palette[i].codepoint == PALETTE_BREAK) {
			flat[len++] = '\n';
		} else {
			uint8_t utf8[5];
			int nbytes = utf8Encode(palette[i].codepoint, utf8);
			memcpy(&flat[len], utf8, nbytes);
			len += nbytes;
			flat[len++] = ' ';
		}
	}
	/* Load into buffer rows (split on '\n'). */
	int start = 0;
	for (int i = 0; i < len; i++) {
		if (flat[i] == '\n') {
			appendRowRaw(buf, &flat[start], i - start);
			start = i + 1;
		}
	}
	if (start < len)
		appendRowRaw(buf, &flat[start], len - start);
	buf->read_only = 1;
}

/* Return the byte offset in `row` of the first non-space character
at or after byte position `from`.  Returns row->size if none. */
static int skipSpacesForward(erow *row, int from) {
	while (from < row->size && row->chars[from] == ' ')
		from++;
	return from;
}

/* Return the byte offset of the first non-space character at or
before `from`, walking backwards.  Returns 0 if the row begins
with a non-space, or the earliest non-space position found. */
static int skipSpacesBackward(erow *row, int from) {
	while (from > 0 && row->chars[from] == ' ')
		from--;
	/* If we landed on a continuation byte, back up to the lead byte. */
	while (from > 0 && (row->chars[from] & 0xC0) == 0x80)
		from--;
	return from;
}

/* After a cursor movement, snap cx to a non-space character so the
cursor always rests on an actual symbol.  `direction` should be
-1 after a backward movement, +1 after a forward movement, and
0 for neutral (initial placement, vertical moves). */
static void snapToSymbol(struct buffer *buf, int direction) {
	erow *row = &buf->row[buf->cy];
	if (row->size == 0)
		return;

	/* Already on a non-space character: nothing to do. */
	if (buf->cx < row->size && row->chars[buf->cx] != ' ')
		return;

	if (direction < 0) {
		/* Backward movement: try backward first */
		int bwd = skipSpacesBackward(row, buf->cx);
		if (bwd < row->size && row->chars[bwd] != ' ') {
			buf->cx = bwd;
			return;
		}
		int fwd = skipSpacesForward(row, buf->cx);
		if (fwd < row->size)
			buf->cx = fwd;
	} else {
		/* Forward or neutral: try forward first */
		int fwd = skipSpacesForward(row, buf->cx);
		if (fwd < row->size) {
			buf->cx = fwd;
			return;
		}
		int bwd = skipSpacesBackward(row, buf->cx);
		if (bwd < row->size && row->chars[bwd] != ' ')
			buf->cx = bwd;
	}
}

/* Close the palette popup and restore window focus to the buffer that
was active when the palette was opened.
If `origin` still inhabits a visible window, focus that window.
Otherwise (typically when the palette was invoked from the
minibuffer, which is not in any window) fall back to whichever
window held focus before the palette opened: clamped to the
current window count, since windows may have been closed.
Updates E.windows[*]->focused and E.buf. */
static void restoreFocusTo(struct buffer *origin, int origin_win) {
	closeSpecialBuffer(PALETTE_BUF_NAME);
	int ow = findBufferWindow(origin);
	int target = (ow >= 0) ? ow :
				 (origin_win < E.nwindows ? origin_win : 0);
	for (int i = 0; i < E.nwindows; i++)
		E.windows[i]->focused = (i == target);
	E.buf = origin;
}

void expandPalette(void) {
	/* Remember the invoking buffer so we can insert into it later. */
	struct buffer *origin = E.buf;
	int origin_win = windowFocusedIdx();

	/* Create or reuse the palette buffer */
	struct buffer *pbuf = findOrCreateSpecialBuffer(PALETTE_BUF_NAME);
	populatePaletteBuffer(pbuf);
	pbuf->word_wrap = 1;
	/* Place cursor on the first symbol (first entry with default_sel) */
	pbuf->cx = 0;
	pbuf->cy = 0;
	pbuf->markx = -1;
	pbuf->marky = -1;
	pbuf->mark_active = 0;
	bufferEnsureRow(pbuf);
	updateBuffer(pbuf);
	showPopupBuffer(pbuf);
	/* Transfer focus to the palette window */
	int palette_win = findBufferWindow(pbuf);
	int from_minibuf = (origin == E.minibuf);
	if (palette_win >= 0) {
		if (!from_minibuf) {
			E.windows[origin_win]->cx = origin->cx;
			E.windows[origin_win]->cy = origin->cy;
		}
		E.windows[origin_win]->focused = 0;
		E.windows[palette_win]->focused = 1;
		E.windows[palette_win]->cx = pbuf->cx;
		E.windows[palette_win]->cy = pbuf->cy;
		E.buf = pbuf;
	}
	/* Snap to the first symbol */
	snapToSymbol(pbuf, 0);

	/* ---- Modal key loop ---- */
	for (;;) {
		refreshScreen();
		int key = readKey();
		if (key == -1)
			continue;
		recordKey(key);

		/* Enter: read symbol at cursor and insert into origin */
		if (key == '\r') {
			if (pbuf->cy < pbuf->numrows) {
				erow *row = &pbuf->row[pbuf->cy];
				int cx = pbuf->cx;
				if (cx < row->size && row->chars[cx] != ' ') {
					int nbytes =
						utf8_nBytes(row->chars[cx]);
					if (cx + nbytes <= row->size) {
						/* Stash the UTF-8 bytes */
						memcpy(E.unicode,
						       &row->chars[cx], nbytes);
						E.nunicode = nbytes;
						restoreFocusTo(origin,
							       origin_win);
						if (rejectIfReadOnly(E.buf))
							return;
						/* Insert the symbol */
						insertUnicode(1);
						return;
					}
				}
			}
			/* Nothing valid under cursor: just beep / ignore */
			continue;
		}
		/* Cancel */
		if (key == CTRL('g')) {
			restoreFocusTo(origin, origin_win);
			setStatusMessage("Canceled.");
			return;
		}
		/* Navigation keys: dispatch normally then snap to symbol */
		int cmd = resolveBinding(key);
		int snap_dir = 0;
		if (cmd != CMD_NONE) {
			/* Only allow movement commands in the palette */
			switch (cmd) {
			case CMD_FORWARD_CHAR:
				moveCursor(KEY_ARROW_RIGHT, 0);
				snap_dir = 1;
				break;
			case CMD_BACKWARD_CHAR:
				moveCursor(KEY_ARROW_LEFT, 0);
				snap_dir = -1;
				break;
			case CMD_NEXT_LINE:
				moveCursor(KEY_ARROW_DOWN, 0);
				break;
			case CMD_PREV_LINE:
				moveCursor(KEY_ARROW_UP, 0);
				break;
			case CMD_HOME:
				beginningOfLine();
				break;
			case CMD_END:
				endOfLine(0);
				snap_dir = -1;
				break;
			case CMD_BEG_OF_FILE:
				pbuf->cy = 0;
				pbuf->cx = 0;
				break;
			case CMD_END_OF_FILE:
				/* bufferEnsureRow ran before this loop and
				 * nothing in it resets rows, so numrows >= 1
				 * holds throughout (#105). */
				pbuf->cy = pbuf->numrows - 1;
				pbuf->cx = 0;
				endOfLine(0);
				snap_dir = -1;
				break;
			case CMD_PAGE_UP:
				pageUp(0);
				break;
			case CMD_PAGE_DOWN:
				pageDown(0);
				break;
			case CMD_FORWARD_WORD:
				forwardWord(0);
				snap_dir = 1;
				break;
			case CMD_BACKWARD_WORD:
				backWord(0);
				snap_dir = -1;
				break;
			default:
				/* Ignore editing / other commands */
				break;
			}
		}
		clampPositions(pbuf);
		snapToSymbol(pbuf, snap_dir);

		erow *row = &pbuf->row[pbuf->cy];
		if (pbuf->cx < row->size && row->chars[pbuf->cx] != ' ') {
			uint32_t cp = utf8Decode(row->chars, pbuf->cx);
			for (int i = 0; i < palette_size; i++) {
				if (palette[i].codepoint == cp &&
				    palette[i].name) {
					setStatusMessage("%s", palette[i].name);
					break;
				}
			}
		}
	}
}
