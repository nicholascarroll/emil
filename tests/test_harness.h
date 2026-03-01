/* test_harness.h — Shared setup for Strategy C fat-binary tests.
 *
 * Provides initTestEditor() which mirrors main.c's initEditor()
 * but uses fixed screen dimensions and skips terminal setup. */

#ifndef TEST_HARNESS_H
#define TEST_HARNESS_H

#include "emil.h"
#include "buffer.h"
#include "keymap.h"
#include "history.h"
#include "undo.h"
#include <string.h>
#include <stdlib.h>

/* E is defined in stubs.c (which replaces main.o) */
extern struct editorConfig E;

/* Set up a minimal but valid editor state. Call once at test start. */
static void initTestEditor(void) {
	memset(&E, 0, sizeof(E));
	E.screencols = 80;
	E.screenrows = 24;
	E.kill = NULL;
	E.rectKill = NULL;
	E.windows = malloc(sizeof(struct editorWindow *));
	E.windows[0] = calloc(1, sizeof(struct editorWindow));
	E.windows[0]->focused = 1;
	E.nwindows = 1;
	E.headbuf = NULL;
	E.recording = 0;
	E.macro.nkeys = 0;
	E.macro.keys = NULL;
	E.micro = 0;
	E.playback = 0;
	E.kill_ring_pos = -1;
	E.macro_depth = 0;
	memset(E.registers, 0, sizeof(E.registers));
	setupCommands(&E);

	initHistory(&E.file_history);
	initHistory(&E.command_history);
	initHistory(&E.shell_history);
	initHistory(&E.search_history);
	initHistory(&E.kill_history);
}

/* Create a buffer with one line of content and wire it into E. */
static struct editorBuffer *make_test_buffer(const char *line) {
	struct editorBuffer *buf = newBuffer();
	if (line && *line)
		editorInsertRow(buf, 0, (char *)line, strlen(line));
	buf->cx = 0;
	buf->cy = 0;
	buf->dirty = 0;
	clearUndosAndRedos(buf);

	/* Wire into editor state */
	E.buf = buf;
	E.headbuf = buf;
	E.windows[0]->buf = buf;
	return buf;
}

/* Create a buffer with multiple lines and wire it into E. */
static struct editorBuffer *make_test_buffer_lines(const char **lines, int n) {
	struct editorBuffer *buf = newBuffer();
	for (int i = 0; i < n; i++)
		editorInsertRow(buf, i, (char *)lines[i], strlen(lines[i]));
	buf->cx = 0;
	buf->cy = 0;
	buf->dirty = 0;
	clearUndosAndRedos(buf);

	E.buf = buf;
	E.headbuf = buf;
	E.windows[0]->buf = buf;
	return buf;
}

/* Get row content as string (safe for assertion). */
static const char *row_str(struct editorBuffer *buf, int row) {
	if (row >= buf->numrows)
		return "";
	return (const char *)buf->row[row].chars;
}

#endif /* TEST_HARNESS_H */
