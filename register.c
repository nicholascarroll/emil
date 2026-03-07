#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "emil.h"
#include "buffer.h"
#include "message.h"
#include "region.h"
#include "register.h"
#include "unicode.h"
#include "unused.h"
#include "display.h"
#include "terminal.h"
#include "util.h"
#include "window.h"

extern struct editorConfig E;

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

/* ---- Special buffer helpers (mirroring *Completions* pattern) ---- */

static struct editorBuffer *findNamedBuffer(const char *name) {
	for (struct editorBuffer *b = E.headbuf; b != NULL; b = b->next) {
		if (b->filename && strcmp(b->filename, name) == 0)
			return b;
	}
	return NULL;
}

static struct editorBuffer *findOrCreateNamedBuffer(const char *name) {
	struct editorBuffer *buf = findNamedBuffer(name);
	if (buf)
		return buf;
	buf = newBuffer();
	buf->filename = xstrdup(name);
	buf->special_buffer = 1;
	buf->next = E.headbuf;
	E.headbuf = buf;
	return buf;
}

static void clearNamedBuffer(struct editorBuffer *buf) {
	while (buf->numrows > 0)
		editorDelRow(buf, 0);
}

static void closeNamedBuffer(const char *name) {
	struct editorBuffer *target = NULL;
	struct editorBuffer *prev = NULL;

	for (struct editorBuffer *b = E.headbuf; b != NULL;
	     prev = b, b = b->next) {
		if (b->filename && strcmp(b->filename, name) == 0) {
			target = b;
			break;
		}
	}
	if (!target)
		return;

	int win = findBufferWindow(target);
	if (win >= 0 && E.nwindows > 1)
		editorDestroyWindow(win);

	if (prev)
		prev->next = target->next;
	else
		E.headbuf = target->next;

	if (E.buf == target)
		E.buf = target->next ? target->next : E.headbuf;
	if (E.lastVisitedBuffer == target)
		E.lastVisitedBuffer = NULL;

	destroyBuffer(target);
}

static void showBufferInWindow(struct editorBuffer *buf) {
	int win_idx = findBufferWindow(buf);
	if (win_idx >= 0)
		return; /* already visible */

	int new_idx = E.nwindows;
	editorCreateWindow();
	E.windows[new_idx]->buf = buf;
	E.windows[new_idx]->focused = 0;

	/* Keep focus on the original window */
	for (int i = 0; i < E.nwindows; i++)
		E.windows[i]->focused = (i == 0);

	/* Size the popup: content height + a little padding */
	extern int minibuffer_height;
	extern const int statusbar_height;
	int comp_height = buf->numrows + 2;
	int total_height = E.screenrows - minibuffer_height -
			   (statusbar_height * E.nwindows);
	int non_popup = E.nwindows - 1;
	int min_others = non_popup * 3;
	int max_popup = total_height - min_others;
	if (comp_height > max_popup)
		comp_height = max_popup;
	if (comp_height < 3)
		comp_height = 3;

	int remaining = total_height - comp_height;
	int per_win = remaining / non_popup;

	win_idx = findBufferWindow(buf);
	for (int i = 0; i < E.nwindows; i++) {
		if (i == win_idx)
			E.windows[i]->height = comp_height;
		else
			E.windows[i]->height = per_win;
	}
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

static void showRegisterPreview(struct editorConfig *ed) {
	struct editorBuffer *prev =
		findOrCreateNamedBuffer("*Register Preview*");
	clearNamedBuffer(prev);
	prev->read_only = 1;
	prev->word_wrap = 0;

	for (int i = 0; i < 127; i++) {
		if (ed->registers[i].rtype == REGISTER_NULL)
			continue;

		char name[8];
		formatRegName(name, sizeof(name), i);
		char line[256];

		switch (ed->registers[i].rtype) {
		case REGISTER_POINT: {
			struct editorPoint *pt = &ed->registers[i].data.point;
			const char *fname = pt->buf->filename ?
						    pt->buf->filename :
						    "*scratch*";
			snprintf(
				line, sizeof(line),
				"%s: buffer position: buffer %s, line %d col %d",
				name, fname, pt->cy + 1, pt->cx);
			break;
		}
		case REGISTER_TEXT:
			if (ed->registers[i].data.text.is_rectangle) {
				snprintf(
					line, sizeof(line),
					"%s: rectangle (w:%d h:%d) starting with '%.60s",
					name,
					ed->registers[i].data.text.rect_width,
					ed->registers[i].data.text.rect_height,
					ed->registers[i].data.text.str);
			} else {
				snprintf(line, sizeof(line),
					 "%s: text starting with '%.60s", name,
					 ed->registers[i].data.text.str);
			}
			break;
		case REGISTER_NULL:
			break;
		}

		editorInsertRow(prev, prev->numrows, line, (int)strlen(line));
	}

	if (prev->numrows == 0)
		editorInsertRow(prev, 0, "(no registers set)", 18);

	showBufferInWindow(prev);
}

static void closeRegisterPreview(void) {
	closeNamedBuffer("*Register Preview*");
}

/* ---- Output buffer (shown after selection) ---- */

static void showRegisterOutput(struct editorConfig *ed, int reg) {
	/* Close the preview first */
	closeRegisterPreview();

	char name[8];
	formatRegName(name, sizeof(name), reg);

	struct editorBuffer *out = findOrCreateNamedBuffer("*Output*");
	clearNamedBuffer(out);
	out->read_only = 1;
	out->word_wrap = 0;

	char header[256];

	switch (ed->registers[reg].rtype) {
	case REGISTER_NULL:
		snprintf(header, sizeof(header), "Register %s is empty.", name);
		editorInsertRow(out, 0, header, (int)strlen(header));
		break;
	case REGISTER_POINT: {
		struct editorPoint *pt = &ed->registers[reg].data.point;
		const char *fname = pt->buf->filename ? pt->buf->filename :
							"*scratch*";
		snprintf(
			header, sizeof(header),
			"Register %s contains the buffer position: buffer %s, line %d col %d",
			name, fname, pt->cy + 1, pt->cx);
		editorInsertRow(out, 0, header, (int)strlen(header));
		break;
	}
	case REGISTER_TEXT:
		if (ed->registers[reg].data.text.is_rectangle) {
			snprintf(header, sizeof(header),
				 "Register %s contains the rectangle:", name);
		} else {
			snprintf(header, sizeof(header),
				 "Register %s contains the text:", name);
		}
		editorInsertRow(out, 0, header, (int)strlen(header));

		/* Add the text content line by line */
		{
			const char *s =
				(const char *)ed->registers[reg].data.text.str;
			while (s && *s) {
				const char *nl = strchr(s, '\n');
				int len;
				if (nl) {
					len = (int)(nl - s);
					editorInsertRow(out, out->numrows,
							(char *)s, len);
					s = nl + 1;
				} else {
					len = (int)strlen(s);
					editorInsertRow(out, out->numrows,
							(char *)s, len);
					break;
				}
			}
		}
		break;
	}

	showBufferInWindow(out);
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
	/* Phase 1: show preview of all registers */
	showRegisterPreview(ed);

	/* Prompt for register name (refreshScreen in the loop shows preview) */
	int reg = getRegisterName("View register");
	if (reg == 0x07) {
		closeRegisterPreview();
		editorSetStatusMessage(msg_quit);
		return;
	}

	/* Phase 2: show detailed output for the chosen register */
	showRegisterOutput(ed, reg);
}
