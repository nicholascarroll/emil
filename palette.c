#include "palette.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "keymap.h"
#include "message.h"
#include "motion.h"
#include "terminal.h"
#include "undo.h"
#include "unicode.h"
#include "window.h"
#include <string.h>

extern struct config E;

/* Functional design is issue #84 */

/* If we stick to linear scan we don't have to worry about sort order.
 * TODO does this cache?
 */
const PaletteEntry palette[] = {
	/* Punctuation */
	{ 0x2014, { 0xE2, 0x80, 0x94, 0x00 }, 3, 1, true },  // —
	{ 0x2013, { 0xE2, 0x80, 0x93, 0x00 }, 3, 1, false }, // –
	{ 0x2026, { 0xE2, 0x80, 0xA6, 0x00 }, 3, 1, false }, // …
	{ 0x201C, { 0xE2, 0x80, 0x9C, 0x00 }, 3, 1, false }, // "
	{ 0x201D, { 0xE2, 0x80, 0x9D, 0x00 }, 3, 1, false }, // "
	{ 0x2018, { 0xE2, 0x80, 0x98, 0x00 }, 3, 1, false }, // '
	{ 0x2019, { 0xE2, 0x80, 0x99, 0x00 }, 3, 1, false }, // '
	{ 0x00AB, { 0xC2, 0xAB, 0x00 }, 2, 1, false },	     // «
	{ 0x00BB, { 0xC2, 0xBB, 0x00 }, 2, 1, false },	     // »
	{ 0x2039, { 0xE2, 0x80, 0xB9, 0x00 }, 3, 1, false }, // ‹
	{ 0x203A, { 0xE2, 0x80, 0xBA, 0x00 }, 3, 1, false }, // ›

	/* Emoji */
	{ PALETTE_BREAK, { '\n', 0 }, 1, 0, false },
	{ 0x1F44D, { 0xF0, 0x9F, 0x91, 0x8D, 0x00 }, 4, 2, true },  // 👍
	{ 0x1F44E, { 0xF0, 0x9F, 0x91, 0x8E, 0x00 }, 4, 2, false }, // 👎
	{ 0x1F440, { 0xF0, 0x9F, 0x91, 0x80, 0x00 }, 4, 2, false }, // 👀
	{ 0x1F44B, { 0xF0, 0x9F, 0x91, 0x8B, 0x00 }, 4, 2, false }, // 👋
	{ 0x1F44C, { 0xF0, 0x9F, 0x91, 0x8C, 0x00 }, 4, 2, false }, // 👌
	{ 0x1F600, { 0xF0, 0x9F, 0x98, 0x80, 0x00 }, 4, 2, false }, // 😀
	{ 0x1F602, { 0xF0, 0x9F, 0x98, 0x82, 0x00 }, 4, 2, false }, // 😂
	{ 0x1F60A, { 0xF0, 0x9F, 0x98, 0x8A, 0x00 }, 4, 2, false }, // 😊
	{ 0x1F609, { 0xF0, 0x9F, 0x98, 0x89, 0x00 }, 4, 2, false }, // 😉
	{ 0x1F60D, { 0xF0, 0x9F, 0x98, 0x8D, 0x00 }, 4, 2, false }, // 😍
	{ 0x1F60E, { 0xF0, 0x9F, 0x98, 0x8E, 0x00 }, 4, 2, false }, // 😎
	{ 0x1F622, { 0xF0, 0x9F, 0x98, 0xA2, 0x00 }, 4, 2, false }, // 😢
	{ 0x1F62D, { 0xF0, 0x9F, 0x98, 0xAD, 0x00 }, 4, 2, false }, // 😭
	{ 0x1F630, { 0xF0, 0x9F, 0x98, 0xB0, 0x00 }, 4, 2, false }, // 😰
	{ 0x1F633, { 0xF0, 0x9F, 0x98, 0xB3, 0x00 }, 4, 2, false }, // 😳
	{ 0x1F923, { 0xF0, 0x9F, 0xA4, 0xA3, 0x00 }, 4, 2, false }, // 🤣
	{ 0x1F973, { 0xF0, 0x9F, 0xA5, 0xB3, 0x00 }, 4, 2, false }, // 🥳
	{ 0x1F977, { 0xF0, 0x9F, 0xA5, 0xB7, 0x00 }, 4, 2, false }, // 🥷
	{ 0x1F97A, { 0xF0, 0x9F, 0xA5, 0xBA, 0x00 }, 4, 2, false }, // 🥺
	{ 0x1F382, { 0xF0, 0x9F, 0x8E, 0x82, 0x00 }, 4, 2, false }, // 🎂
	{ 0x1F389, { 0xF0, 0x9F, 0x8E, 0x89, 0x00 }, 4, 2, false }, // 🎉
	{ 0x1F4A1, { 0xF0, 0x9F, 0x92, 0xA1, 0x00 }, 4, 2, false }, // 💡
	{ 0x1F525, { 0xF0, 0x9F, 0x94, 0xA5, 0x00 }, 4, 2, false }, // 🔥
	{ 0x1F680, { 0xF0, 0x9F, 0x9A, 0x80, 0x00 }, 4, 2, false }, // 🚀
	{ 0x1F517, { 0xF0, 0x9F, 0x94, 0x97, 0x00 }, 4, 2, false }, // 🔗
	{ 0x1F48B, { 0xF0, 0x9F, 0x92, 0x8B, 0x00 }, 4, 2, false }, // 💋
	{ 0x1F495, { 0xF0, 0x9F, 0x92, 0x95, 0x00 }, 4, 2, false }, // 💕
	{ 0x1F496, { 0xF0, 0x9F, 0x92, 0x96, 0x00 }, 4, 2, false }, // 💖
	{ 0x1F970, { 0xF0, 0x9F, 0xA5, 0xB0, 0x00 }, 4, 2, false }, // 🥰
	{ 0x1F498, { 0xF0, 0x9F, 0x92, 0x98, 0x00 }, 4, 2, false }, // 💘
	{ 0x1F494, { 0xF0, 0x9F, 0x92, 0x94, 0x00 }, 4, 2, false }, // 💔
	{ 0x1F337, { 0xF0, 0x9F, 0x8C, 0xB7, 0x00 }, 4, 2, false }, // 🌷
	{ 0x1F339, { 0xF0, 0x9F, 0x8C, 0xB9, 0x00 }, 4, 2, false }, // 🌹
	{ 0x1F33B, { 0xF0, 0x9F, 0x8C, 0xBB, 0x00 }, 4, 2, false }, // 🌻
	{ 0x1F33C, { 0xF0, 0x9F, 0x8C, 0xBC, 0x00 }, 4, 2, false }, // 🌼
	{ 0x1F408, { 0xF0, 0x9F, 0x90, 0x88, 0x00 }, 4, 2, false }, // 🐈
	{ 0x1F415, { 0xF0, 0x9F, 0x90, 0x95, 0x00 }, 4, 2, false }, // 🐕
	{ 0x2705, { 0xE2, 0x9C, 0x85, 0x00 }, 3, 2, false },	    // ✅
	{ 0x274C, { 0xE2, 0x9D, 0x8C, 0x00 }, 3, 2, false },	    // ❌
	{ 0x2728, { 0xE2, 0x9C, 0xA8, 0x00 }, 3, 2, false },	    // ✨
	{ 0x26A1, { 0xE2, 0x9A, 0xA1, 0x00 }, 3, 2, false },	    // ⚡
	{ 0x2B50, { 0xE2, 0xAD, 0x90, 0x00 }, 3, 2, false },	    // ⭐
	{ 0x270B, { 0xE2, 0x9C, 0x8B, 0x00 }, 3, 2, false },	    // ✋
	{ 0x1FAE1, { 0xF0, 0x9F, 0xAB, 0xA1, 0x00 }, 4, 2, false }, // 🫡
	{ 0x1F60F, { 0xF0, 0x9F, 0x98, 0x8F, 0x00 }, 4, 2, false }, // 😏
	{ 0x1F610, { 0xF0, 0x9F, 0x98, 0x90, 0x00 }, 4, 2, false }, // 😐
	{ 0x1F612, { 0xF0, 0x9F, 0x98, 0x92, 0x00 }, 4, 2, false }, // 😒
	{ 0x1F618, { 0xF0, 0x9F, 0x98, 0x98, 0x00 }, 4, 2, false }, // 😘
	{ 0x1F61C, { 0xF0, 0x9F, 0x98, 0x9C, 0x00 }, 4, 2, false }, // 😜
	{ 0x1F61F, { 0xF0, 0x9F, 0x98, 0x9F, 0x00 }, 4, 2, false }, // 😟
	{ 0x1F620, { 0xF0, 0x9F, 0x98, 0xA0, 0x00 }, 4, 2, false }, // 😠
	{ 0x1F621, { 0xF0, 0x9F, 0x98, 0xA1, 0x00 }, 4, 2, false }, // 😡
	{ 0x1F62C, { 0xF0, 0x9F, 0x98, 0xAC, 0x00 }, 4, 2, false }, // 😬
	{ 0x1F625, { 0xF0, 0x9F, 0x98, 0xA5, 0x00 }, 4, 2, false }, // 😥
	{ 0x1F641, { 0xF0, 0x9F, 0x99, 0x81, 0x00 }, 4, 2, false }, // 🙁
	{ 0x1F642, { 0xF0, 0x9F, 0x99, 0x82, 0x00 }, 4, 2, false }, // 🙂
	{ 0x1F914, { 0xF0, 0x9F, 0xA4, 0x94, 0x00 }, 4, 2, false }, // 🤔
	{ 0x1F925, { 0xF0, 0x9F, 0xA4, 0xA5, 0x00 }, 4, 2, false }, // 🤥
	{ 0x1F928, { 0xF0, 0x9F, 0xA4, 0xA8, 0x00 }, 4, 2, false }, // 🤨

	/* Legal */
	{ PALETTE_BREAK, { '\n', 0 }, 1, 0, false },
	{ 0x00A9, { 0xC2, 0xA9, 0x00 }, 2, 1, true },	     // ©
	{ 0x00AE, { 0xC2, 0xAE, 0x00 }, 2, 1, false },	     // ®
	{ 0x2122, { 0xE2, 0x84, 0xA2, 0x00 }, 3, 1, false }, // ™
	{ 0x00A7, { 0xC2, 0xA7, 0x00 }, 2, 1, false },	     // §
	{ 0x00B6, { 0xC2, 0xB6, 0x00 }, 2, 1, false },	     // ¶
	{ 0x2020, { 0xE2, 0x80, 0xA0, 0x00 }, 3, 1, false }, // †
	{ 0x2021, { 0xE2, 0x80, 0xA1, 0x00 }, 3, 1, false }, // ‡

	/* Currency */
	{ PALETTE_BREAK, { '\n', 0 }, 1, 0, false },
	{ 0x20AC, { 0xE2, 0x82, 0xAC, 0x00 }, 3, 1, true },  // €
	{ 0x00A3, { 0xC2, 0xA3, 0x00 }, 2, 1, false },	     // £
	{ 0x00A5, { 0xC2, 0xA5, 0x00 }, 2, 1, false },	     // ¥
	{ 0x20B9, { 0xE2, 0x82, 0xB9, 0x00 }, 3, 1, false }, // ₹
	{ 0x20BF, { 0xE2, 0x82, 0xBF, 0x00 }, 3, 1, false }, // ₿
	{ 0x00A2, { 0xC2, 0xA2, 0x00 }, 2, 1, false },	     // ¢
	{ 0x20A9, { 0xE2, 0x82, 0xA9, 0x00 }, 3, 1, false }, // ₩

	/* Maths */
	{ PALETTE_BREAK, { '\n', 0 }, 1, 0, false },
	{ 0x00B1, { 0xC2, 0xB1, 0x00 }, 2, 1, true },	     // ±
	{ 0x00D7, { 0xC3, 0x97, 0x00 }, 2, 1, false },	     // ×
	{ 0x00F7, { 0xC3, 0xB7, 0x00 }, 2, 1, false },	     // ÷
	{ 0x2260, { 0xE2, 0x89, 0xA0, 0x00 }, 3, 1, false }, // ≠
	{ 0x2248, { 0xE2, 0x89, 0x88, 0x00 }, 3, 1, false }, // ≈
	{ 0x2264, { 0xE2, 0x89, 0xA4, 0x00 }, 3, 1, false }, // ≤
	{ 0x2265, { 0xE2, 0x89, 0xA5, 0x00 }, 3, 1, false }, // ≥
	{ 0x221E, { 0xE2, 0x88, 0x9E, 0x00 }, 3, 1, false }, // ∞
	{ 0x221A, { 0xE2, 0x88, 0x9A, 0x00 }, 3, 1, false }, // √
	{ 0x03C0, { 0xCF, 0x80, 0x00 }, 2, 1, false },	     // π
	{ 0x03BB, { 0xCE, 0xBB, 0x00 }, 2, 1, false },	     // λ
	{ 0x03BC, { 0xCE, 0xBC, 0x00 }, 2, 1, false },	     // μ
	{ 0x03A3, { 0xCE, 0xA3, 0x00 }, 2, 1, false },	     // Σ
	{ 0x03B1, { 0xCE, 0xB1, 0x00 }, 2, 1, false },	     // α
	{ 0x03B2, { 0xCE, 0xB2, 0x00 }, 2, 1, false },	     // β
	{ 0x00B0, { 0xC2, 0xB0, 0x00 }, 2, 1, false },	     // °

	/* Misc */
	{ PALETTE_BREAK, { '\n', 0 }, 1, 0, false },
	{ 0x2022, { 0xE2, 0x80, 0xA2, 0x00 }, 3, 1, true },  // •
	{ 0x25E6, { 0xE2, 0x97, 0xA6, 0x00 }, 3, 1, false }, // ◦
	{ 0x2713, { 0xE2, 0x9C, 0x93, 0x00 }, 3, 1, false }, // ✓
	{ 0x2717, { 0xE2, 0x9C, 0x97, 0x00 }, 3, 1, false }, // ✗
	{ 0x00BD, { 0xC2, 0xBD, 0x00 }, 2, 1, false },	     // ½
	{ 0x00BC, { 0xC2, 0xBC, 0x00 }, 2, 1, false },	     // ¼
	{ 0x00BE, { 0xC2, 0xBE, 0x00 }, 2, 1, false },	     // ¾
	{ 0x00B2, { 0xC2, 0xB2, 0x00 }, 2, 1, false },	     // ²
	{ 0x00B3, { 0xC2, 0xB3, 0x00 }, 2, 1, false },	     // ³
};

const int palette_size = sizeof(palette) / sizeof(palette[0]);

int palette_wcwidth(uint32_t codepoint) {
	for (int i = 0; i < palette_size; i++)
		if (palette[i].codepoint ==
		    codepoint) /* PALETTE_BREAK (0x00) never matches real input */
			return palette[i].width;
	return -1;
}

/* ------------------------------------------------------------------ */
/* Palette popup                                                       */
/* ------------------------------------------------------------------ */

#define PALETTE_BUF_NAME "*Palette*"

/* Populate the palette buffer.  Mirrors the dump_palette utility:
 * concatenate every entry's utf8[] with a trailing space.  The
 * PALETTE_BREAK entries contain '\n', producing line breaks. */
static void populatePaletteBuffer(struct buffer *buf) {
	clearBuffer(buf);
	buf->read_only = 0;

	uint8_t flat[4096];
	int len = 0;
	for (int i = 0; i < palette_size; i++) {
		memcpy(&flat[len], palette[i].utf8, palette[i].utf8_len);
		len += palette[i].utf8_len;
		if (palette[i].codepoint != PALETTE_BREAK)
			flat[len++] = ' ';
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
 * at or after byte position `from`.  Returns row->size if none. */
static int skipSpacesForward(erow *row, int from) {
	while (from < row->size && row->chars[from] == ' ')
		from++;
	return from;
}

/* Return the byte offset of the first non-space character at or
 * before `from`, walking backwards.  Returns 0 if the row begins
 * with a non-space, or the earliest non-space position found. */
static int skipSpacesBackward(erow *row, int from) {
	while (from > 0 && row->chars[from] == ' ')
		from--;
	/* If we landed on a continuation byte, back up to the lead byte. */
	while (from > 0 && (row->chars[from] & 0xC0) == 0x80)
		from--;
	return from;
}

/* After a cursor movement, snap cx to a non-space character so the
 * cursor always rests on an actual symbol.  `direction` should be
 * -1 after a backward movement, +1 after a forward movement, and
 * 0 for neutral (initial placement, vertical moves). */
static void snapToSymbol(struct buffer *buf, int direction) {
	if (buf->cy >= buf->numrows)
		return;
	erow *row = &buf->row[buf->cy];
	if (row->size == 0)
		return;

	/* Already on a non-space character — nothing to do. */
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

						/* Close palette and restore focus */
						closeSpecialBuffer(
							PALETTE_BUF_NAME);

						/* Restore focus to the origin window */
						int ow = findBufferWindow(
							origin);
						if (ow >= 0) {
							for (int i = 0;
							     i < E.nwindows;
							     i++)
								E.windows[i]
									->focused =
									(i ==
									 ow);
						} else {
							/* Origin is the minibuffer (not in
							 * any window).  Just refocus the
							 * window that was active before the
							 * palette was opened. */
							int fw =
								origin_win < E.nwindows ?
									origin_win :
									0;
							for (int i = 0;
							     i < E.nwindows;
							     i++)
								E.windows[i]
									->focused =
									(i ==
									 fw);
						}
						E.buf = origin;

						/* Insert the symbol */
						undoAppendUnicode(E.buf);
						if (E.buf->cy == E.buf->numrows)
							insertRow(
								E.buf,
								E.buf->numrows,
								(const uint8_t
									 *)"",
								0);
						rowInsertUnicode(
							E.buf,
							&E.buf->row[E.buf->cy],
							E.buf->cx);
						E.buf->cx += E.nunicode;
						return;
					}
				}
			}
			/* Nothing valid under cursor — just beep / ignore */
			continue;
		}

		/* Cancel */
		if (key == CTRL('g')) {
			closeSpecialBuffer(PALETTE_BUF_NAME);

			int ow = findBufferWindow(origin);
			if (ow >= 0) {
				for (int i = 0; i < E.nwindows; i++)
					E.windows[i]->focused = (i == ow);
			} else {
				int fw = origin_win < E.nwindows ? origin_win :
								   0;
				for (int i = 0; i < E.nwindows; i++)
					E.windows[i]->focused = (i == fw);
			}
			E.buf = origin;
			setStatusMessage(msg_canceled);
			return;
		}

		/* Navigation keys — dispatch normally then snap to symbol */
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
				pbuf->cy = pbuf->numrows > 0 ?
						   pbuf->numrows - 1 :
						   0;
				pbuf->cx = 0;
				if (pbuf->cy < pbuf->numrows)
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
	}
}
