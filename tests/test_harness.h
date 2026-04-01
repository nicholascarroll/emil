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
extern struct config E;


static void cleanupTestEditor(void) {
    freeHistory(&E.file_history);
    freeHistory(&E.command_history);
    freeHistory(&E.shell_history);
    freeHistory(&E.search_history);
    freeHistory(&E.kill_history);

    if (E.windows) {
        if (E.windows[0]) free(E.windows[0]);
        free(E.windows);
        E.windows = NULL;
    }

    E.buf = NULL;
    E.headbuf = NULL;
}


static void initTestEditor(void) {
    if (E.windows == NULL) {
        E.windows = malloc(sizeof(struct window *));
        E.windows[0] = calloc(1, sizeof(struct window));
    }
    
    E.screencols = 80;
    E.screenrows = 24;
    E.nwindows = 1;
    E.windows[0]->focused = 1;

    /* Initialize histories only if they haven't been */
    initHistory(&E.file_history);
    initHistory(&E.command_history);
    initHistory(&E.shell_history);
    initHistory(&E.search_history);
    initHistory(&E.kill_history);
}


/* Create a buffer with one line of content and wire it into E. */
static struct buffer *make_test_buffer(const char *line) {
	struct buffer *buf = newBuffer();
	if (line && *line)
		insertRow(buf, 0, (char *)line, strlen(line));
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
static struct buffer *make_test_buffer_lines(const char **lines, int n) {
	struct buffer *buf = newBuffer();
	for (int i = 0; i < n; i++)
		insertRow(buf, i, (char *)lines[i], strlen(lines[i]));
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
static const char *row_str(struct buffer *buf, int row) {
	if (row >= buf->numrows)
		return "";
	return (const char *)buf->row[row].chars;
}

#endif /* TEST_HARNESS_H */
