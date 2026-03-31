#include "register.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "message.h"
#include "region.h"
#include "terminal.h"
#include "unicode.h"
#include "unused.h"
#include "util.h"
#include "window.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern struct config E;

static int getRegisterName(char *prompt) {
	int key;
	int psize = stringWidth((uint8_t *)prompt);
	do {
		setStatusMessage("%s:", prompt);
		cursorBottomLine(psize + 2);
		refreshScreen();
		key = readKey();
	} while (key > 127);
	recordKey(key);
	return key;
}

#define GET_REGISTER(vname, prompt)          \
	int vname = getRegisterName(prompt); \
	if (vname == 0x07) {                 \
		setStatusMessage(msg_quit);  \
		return;                      \
	}

static void registerMessage(char *msg, char reg) {
	char str[4];
	if (reg < 32) {
		snprintf(str, sizeof(str), "C-%c", reg + '@');
	} else {
		str[0] = reg;
		str[1] = 0;
	}
	setStatusMessage(msg, str);
}

static void clearRegister(int reg) {
	switch (E.registers[reg].rtype) {
	case REGISTER_TEXT:
		clearText(&E.registers[reg].data.text);
		break;
	case REGISTER_POINT:
	case REGISTER_NULL:
		break;
	}
	E.registers[reg].rtype = REGISTER_NULL;
}

/* ---- Special buffer helpers ---- */

static int bufferAlive(struct buffer *buf) {
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
		if (b == buf)
			return 1;
	}
	return 0;
}

static void closeRegisterPreview(void) {
	closeSpecialBuffer("*Register Preview*");
}

/* Format a register name for display. */
static void formatRegName(char *out, size_t outsz, int reg) {
	if (reg < 32)
		snprintf(out, outsz, "C-%c", reg + '@');
	else {
		out[0] = (char)reg;
		out[1] = 0;
	}
}

/* ---- Register Preview (shown before prompt) ---- */

static void showRegisterPreview(void) {
	struct buffer *prev = findOrCreateSpecialBuffer("*Register Preview*");
	clearBuffer(prev);
	prev->read_only = 1;
	prev->word_wrap = 0;

	for (int i = 0; i < 127; i++) {
		if (E.registers[i].rtype == REGISTER_NULL)
			continue;

		char name[8];
		formatRegName(name, sizeof(name), i);
		char line[256];

		switch (E.registers[i].rtype) {
		case REGISTER_POINT: {
			struct point *pt = &E.registers[i].data.point;
			if (!bufferAlive(pt->buf)) {
				snprintf(line, sizeof(line),
					 "%s: stale point (buffer closed)",
					 name);
			} else {
				const char *fname = pt->buf->filename ?
							    pt->buf->filename :
							    "*scratch*";
				snprintf(
					line, sizeof(line),
					"%s: buffer position: buffer %s, line %d col %d",
					name, fname, pt->cy + 1, pt->cx);
			}
			break;
		}
		case REGISTER_TEXT:
			if (E.registers[i].data.text.is_rectangle) {
				snprintf(
					line, sizeof(line),
					"%s: rectangle (w:%d h:%d) starting with '%.60s",
					name,
					E.registers[i].data.text.rect_width,
					E.registers[i].data.text.rect_height,
					E.registers[i].data.text.str);
			} else {
				snprintf(line, sizeof(line),
					 "%s: text starting with '%.60s", name,
					 E.registers[i].data.text.str);
			}
			break;
		case REGISTER_NULL:
			break;
		}

		insertRow(prev, prev->numrows, line, (int)strlen(line));
	}

	if (prev->numrows == 0)
		insertRow(prev, 0, "(no registers set)", 18);

	showPopupBuffer(prev);
}

/* ---- Output buffer (shown after selection) ---- */

static void showRegisterOutput(int reg) {
	/* Close the preview first */
	closeRegisterPreview();

	char name[8];
	formatRegName(name, sizeof(name), reg);

	struct buffer *out = findOrCreateSpecialBuffer("*Output*");
	clearBuffer(out);
	out->read_only = 1;
	out->word_wrap = 0;

	char header[256];

	switch (E.registers[reg].rtype) {
	case REGISTER_NULL:
		snprintf(header, sizeof(header), "Register %s is empty.", name);
		insertRow(out, 0, header, (int)strlen(header));
		break;
	case REGISTER_POINT: {
		struct point *pt = &E.registers[reg].data.point;
		if (!bufferAlive(pt->buf)) {
			snprintf(header, sizeof(header),
				 "Register %s: stale point (buffer closed)",
				 name);
		} else {
			const char *fname = pt->buf->filename ?
						    pt->buf->filename :
						    "*scratch*";
			snprintf(
				header, sizeof(header),
				"Register %s contains the buffer position: buffer %s, line %d col %d",
				name, fname, pt->cy + 1, pt->cx);
		}
		insertRow(out, 0, header, (int)strlen(header));
		break;
	}
	case REGISTER_TEXT:
		if (E.registers[reg].data.text.is_rectangle) {
			snprintf(header, sizeof(header),
				 "Register %s contains the rectangle:", name);
		} else {
			snprintf(header, sizeof(header),
				 "Register %s contains the text:", name);
		}
		insertRow(out, 0, header, (int)strlen(header));

		/* Add the text content line by line */
		{
			const char *s =
				(const char *)E.registers[reg].data.text.str;
			while (s && *s) {
				const char *nl = strchr(s, '\n');
				int len;
				if (nl) {
					len = (int)(nl - s);
					insertRow(out, out->numrows, (char *)s,
						  len);
					s = nl + 1;
				} else {
					len = (int)strlen(s);
					insertRow(out, out->numrows, (char *)s,
						  len);
					break;
				}
			}
		}
		break;
	}

	showPopupBuffer(out);
}

void jumpToRegister(void) {
	GET_REGISTER(reg, "Jump to register");
	switch (E.registers[reg].rtype) {
	case REGISTER_NULL:
		registerMessage("Nothing in register %s", reg);
		break;
	case REGISTER_TEXT:
		registerMessage("Cannot jump to text in register %s", reg);
		break;
	case REGISTER_POINT:
		if (!bufferAlive(E.registers[reg].data.point.buf)) {
			E.registers[reg].rtype = REGISTER_NULL;
			registerMessage(
				"Buffer for register %s has been killed", reg);
			break;
		}
		if (E.buf == E.registers[reg].data.point.buf) {
			setMarkSilent();
		} else {
			E.buf = E.registers[reg].data.point.buf;
			for (int i = 0; i < E.nwindows; i++) {
				if (E.windows[i]->focused) {
					E.windows[i]->buf = E.buf;
				}
			}
			registerMessage("Jumped to point in register %s", reg);
		}
		struct buffer *buf = E.buf;
		buf->cx = E.registers[reg].data.point.cx;
		buf->cy = E.registers[reg].data.point.cy;
		if (buf->numrows == 0) {
			buf->cx = 0;
			buf->cy = 0;
		} else {
			if (buf->cy >= buf->numrows)
				buf->cy = buf->numrows - 1;
			if (buf->cx > buf->row[buf->cy].size)
				buf->cx = buf->row[buf->cy].size;
		}
		break;
	}
}

void pointToRegister(void) {
	GET_REGISTER(reg, "Point to register");
	clearRegister(reg);
	E.registers[reg].rtype = REGISTER_POINT;
	E.registers[reg].data.point.buf = E.buf;
	E.registers[reg].data.point.cx = E.buf->cx;
	E.registers[reg].data.point.cy = E.buf->cy;
	registerMessage("Saved point to register %s", reg);
}

void regionToRegister(void) {
	if (markInvalid())
		return;
	GET_REGISTER(reg, "Region to register");
	clearRegister(reg);

	struct text saved = E.kill;
	E.kill = (struct text){ 0 };
	copyRegion();
	E.registers[reg].rtype = REGISTER_TEXT;
	E.registers[reg].data.text.str = E.kill.str;
	E.registers[reg].data.text.is_rectangle = 0;
	E.registers[reg].data.text.rect_width = 0;
	E.registers[reg].data.text.rect_height = 0;
	/* Don't free E.kill.str — ownership transferred to register */
	E.kill = saved;
	registerMessage("Saved region to register %s", reg);
}

void rectToRegister(void) {
	if (markInvalid())
		return;
	GET_REGISTER(reg, "Rectangle to register");
	clearRegister(reg);

	struct text saved = E.kill;
	E.kill = (struct text){ 0 };
	copyRectangle();
	E.registers[reg].rtype = REGISTER_TEXT;
	E.registers[reg].data.text = E.kill;
	/* copyRectangle already set is_rectangle, rect_width, rect_height.
	 * Ownership of str transferred to register. */
	E.kill = saved;
	registerMessage("Saved rectangle to register %s", reg);
}

void incrementRegister(void) {
	GET_REGISTER(reg, "Increment register");
	switch (E.registers[reg].rtype) {
	case REGISTER_NULL:
		registerMessage("Nothing in register %s", reg);
		break;
	case REGISTER_TEXT:
		if (E.registers[reg].data.text.is_rectangle) {
			registerMessage(
				"Cannot increment rectangle in register %s",
				reg);
			break;
		}
		{
			struct text saved = E.kill;
			E.kill = (struct text){ 0 };
			copyRegion();
			size_t kill_len = strlen((char *)E.kill.str);
			size_t region_len =
				strlen((char *)E.registers[reg].data.text.str);
			size_t new_len = region_len + kill_len + 1;
			E.registers[reg].data.text.str = xrealloc(
				E.registers[reg].data.text.str, new_len);
			memcpy(E.registers[reg].data.text.str + region_len,
			       E.kill.str, kill_len + 1);
			clearText(&E.kill);
			E.kill = saved;
			registerMessage("Added region to register %s", reg);
		}
		break;
	case REGISTER_POINT:
		registerMessage("Cannot increment point in register %s", reg);
		break;
	}
}

void insertRegister(void) {
	GET_REGISTER(reg, "Insert register");
	switch (E.registers[reg].rtype) {
	case REGISTER_NULL:
		registerMessage("Nothing in register %s", reg);
		break;
	case REGISTER_TEXT:
		if (E.registers[reg].data.text.is_rectangle) {
			/* Rectangle insertion: temporarily swap E.kill */
			struct text saved = E.kill;
			E.kill = E.registers[reg].data.text;
			E.kill.str = (uint8_t *)xstrdup(
				(char *)E.registers[reg].data.text.str);
			yankRectangle();
			clearText(&E.kill);
			E.kill = saved;
			registerMessage("Inserted rectangle register %s", reg);
		} else {
			struct text saved = E.kill;
			E.kill.str = E.registers[reg].data.text.str;
			E.kill.is_rectangle = 0;
			E.kill.rect_width = 0;
			E.kill.rect_height = 0;
			yank(1);
			E.kill = saved;
			registerMessage("Inserted string register %s", reg);
		}
		break;
	case REGISTER_POINT:
		registerMessage("Cannot insert point in register %s", reg);
		break;
	}
}

void viewRegister(void) {
	/* Phase 1: show preview of all registers */
	showRegisterPreview();

	/* Prompt for register name (refreshScreen in the loop shows preview) */
	int reg = getRegisterName("View register");
	if (reg == 0x07) {
		closeRegisterPreview();
		setStatusMessage(msg_quit);
		return;
	}

	/* Phase 2: show detailed output for the chosen register */
	showRegisterOutput(reg);
}
