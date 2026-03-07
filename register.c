#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emil.h"
#include "message.h"
#include "region.h"
#include "register.h"
#include "unicode.h"
#include "unused.h"
#include "display.h"
#include "terminal.h"
#include "util.h"

static int getRegisterName(char *prompt) {
	int key;
	int psize = stringWidth((uint8_t *)prompt);
	do {
		editorSetStatusMessage("%s:", prompt);
		cursorBottomLine(psize + 2);
		refreshScreen();
		key = editorReadKey();
	} while (key > 127);
	editorRecordKey(key);
	return key;
}

#define GET_REGISTER(vname, prompt)               \
	int vname = getRegisterName(prompt);      \
	if (vname == 0x07) {                      \
		editorSetStatusMessage(msg_quit); \
		return;                           \
	}

static void registerMessage(char *msg, char reg) {
	char str[4];
	if (reg < 32) {
		snprintf(str, sizeof(str), "C-%c", reg + '@');
	} else {
		str[0] = reg;
		str[1] = 0;
	}
	editorSetStatusMessage(msg, str);
}

static void clearRegister(struct editorConfig *ed, int reg) {
	switch (ed->registers[reg].rtype) {
	case REGISTER_TEXT:
		clearEditorText(&ed->registers[reg].data.text);
		break;
	case REGISTER_POINT:
	case REGISTER_NULL:
		break;
	}
	ed->registers[reg].rtype = REGISTER_NULL;
}

void editorJumpToRegister(struct editorConfig *ed) {
	GET_REGISTER(reg, "Jump to register");
	switch (ed->registers[reg].rtype) {
	case REGISTER_NULL:
		registerMessage("Nothing in register %s", reg);
		break;
	case REGISTER_TEXT:
		registerMessage("Cannot jump to text in register %s", reg);
		break;
	case REGISTER_POINT:
		if (ed->buf == ed->registers[reg].data.point.buf) {
			editorSetMarkSilent();
		} else {
			ed->buf = ed->registers[reg].data.point.buf;
			for (int i = 0; i < ed->nwindows; i++) {
				if (ed->windows[i]->focused) {
					ed->windows[i]->buf = ed->buf;
				}
			}
			registerMessage("Jumped to point in register %s", reg);
		}
		struct editorBuffer *buf = ed->buf;
		buf->cx = ed->registers[reg].data.point.cx;
		buf->cy = ed->registers[reg].data.point.cy;
		if (buf->cy >= buf->numrows)
			buf->cy = buf->numrows - 1;
		if (buf->cy < 0)
			buf->cy = 0;
		if (buf->cx > buf->row[buf->cy].size)
			buf->cx = buf->row[buf->cy].size;
		break;
	}
}

void editorPointToRegister(struct editorConfig *ed) {
	GET_REGISTER(reg, "Point to register");
	clearRegister(ed, reg);
	ed->registers[reg].rtype = REGISTER_POINT;
	ed->registers[reg].data.point.buf = ed->buf;
	ed->registers[reg].data.point.cx = ed->buf->cx;
	ed->registers[reg].data.point.cy = ed->buf->cy;
	registerMessage("Saved point to register %s", reg);
}

void editorRegionToRegister(struct editorConfig *ed,
			    struct editorBuffer *bufr) {
	if (markInvalid())
		return;
	GET_REGISTER(reg, "Region to register");
	clearRegister(ed, reg);

	struct editorText saved = ed->kill;
	ed->kill = (struct editorText){ 0 };
	editorCopyRegion(ed, bufr);
	ed->registers[reg].rtype = REGISTER_TEXT;
	ed->registers[reg].data.text.str = ed->kill.str;
	ed->registers[reg].data.text.is_rectangle = 0;
	ed->registers[reg].data.text.rect_width = 0;
	ed->registers[reg].data.text.rect_height = 0;
	/* Don't free ed->kill.str — ownership transferred to register */
	ed->kill = saved;
	registerMessage("Saved region to register %s", reg);
}

void editorRectToRegister(struct editorConfig *ed, struct editorBuffer *bufr) {
	if (markInvalid())
		return;
	GET_REGISTER(reg, "Rectangle to register");
	clearRegister(ed, reg);

	struct editorText saved = ed->kill;
	ed->kill = (struct editorText){ 0 };
	editorCopyRectangle(ed, bufr);
	ed->registers[reg].rtype = REGISTER_TEXT;
	ed->registers[reg].data.text = ed->kill;
	/* editorCopyRectangle already set is_rectangle, rect_width, rect_height.
	 * Ownership of str transferred to register. */
	ed->kill = saved;
	registerMessage("Saved rectangle to register %s", reg);
}

void editorIncrementRegister(struct editorConfig *ed,
			     struct editorBuffer *bufr) {
	GET_REGISTER(reg, "Increment register");
	switch (ed->registers[reg].rtype) {
	case REGISTER_NULL:
		registerMessage("Nothing in register %s", reg);
		break;
	case REGISTER_TEXT:
		if (ed->registers[reg].data.text.is_rectangle) {
			registerMessage(
				"Cannot increment rectangle in register %s",
				reg);
			break;
		}
		{
			struct editorText saved = ed->kill;
			ed->kill = (struct editorText){ 0 };
			editorCopyRegion(ed, bufr);
			size_t kill_len = strlen((char *)ed->kill.str);
			size_t region_len = strlen(
				(char *)ed->registers[reg].data.text.str);
			size_t new_len = region_len + kill_len + 1;
			ed->registers[reg].data.text.str = xrealloc(
				ed->registers[reg].data.text.str, new_len);
			memcpy(ed->registers[reg].data.text.str + region_len,
			       ed->kill.str, kill_len + 1);
			clearEditorText(&ed->kill);
			ed->kill = saved;
			registerMessage("Added region to register %s", reg);
		}
		break;
	case REGISTER_POINT:
		registerMessage("Cannot increment point in register %s", reg);
		break;
	}
}

void editorInsertRegister(struct editorConfig *ed, struct editorBuffer *bufr) {
	GET_REGISTER(reg, "Insert register");
	switch (ed->registers[reg].rtype) {
	case REGISTER_NULL:
		registerMessage("Nothing in register %s", reg);
		break;
	case REGISTER_TEXT:
		if (ed->registers[reg].data.text.is_rectangle) {
			/* Rectangle insertion: temporarily swap ed->kill */
			struct editorText saved = ed->kill;
			ed->kill = ed->registers[reg].data.text;
			ed->kill.str = (uint8_t *)xstrdup(
				(char *)ed->registers[reg].data.text.str);
			editorYankRectangle(ed, bufr);
			clearEditorText(&ed->kill);
			ed->kill = saved;
			registerMessage("Inserted rectangle register %s", reg);
		} else {
			struct editorText saved = ed->kill;
			ed->kill.str = ed->registers[reg].data.text.str;
			ed->kill.is_rectangle = 0;
			ed->kill.rect_width = 0;
			ed->kill.rect_height = 0;
			editorYank(ed, bufr, 1);
			ed->kill = saved;
			registerMessage("Inserted string register %s", reg);
		}
		break;
	case REGISTER_POINT:
		registerMessage("Cannot insert point in register %s", reg);
		break;
	}
}

void editorViewRegister(struct editorConfig *ed,
			struct editorBuffer *UNUSED(bufr)) {
	GET_REGISTER(reg, "View register");
	char str[4];
	if (reg < 32) {
		snprintf(str, sizeof(str), "C-%c", reg + '@');
	} else {
		str[0] = reg;
		str[1] = 0;
	}
	switch (ed->registers[reg].rtype) {
	case REGISTER_NULL:
		editorSetStatusMessage("Register %s is empty.", str);
		break;
	case REGISTER_TEXT:
		if (ed->registers[reg].data.text.is_rectangle) {
			editorSetStatusMessage(
				"%s (rect): w: %d h: %d \"%.50s\"", str,
				ed->registers[reg].data.text.rect_width,
				ed->registers[reg].data.text.rect_height,
				ed->registers[reg].data.text.str);
		} else {
			editorSetStatusMessage(
				"%s (text): %.60s", str,
				ed->registers[reg].data.text.str);
		}
		break;
	case REGISTER_POINT:;
		struct editorPoint *pt = &ed->registers[reg].data.point;
		editorSetStatusMessage("%s (point): %.20s %d:%d", str,
				       pt->buf->filename, pt->cy + 1, pt->cx);
		break;
	}
}
