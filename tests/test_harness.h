/* test_harness.h — Shared setup for Strategy C fat-binary tests.
 *
 * Provides initTestEditor() which mirrors main.c's initEditor()
 * but uses fixed screen dimensions and skips terminal setup.
 *
 * IMPORTANT: cleanupTestEditor must free ALL resources reachable from E.
 * initTestEditor must fully reset E to a known state.  Together they
 * ensure no leaked memory, no dangling pointers, and no state bleed
 * between tests — which is required for sanitizer builds to pass
 * cleanly. */

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
    /* Free the buffer list.  Tests are expected to destroyBuffer()
     * their own buffers, but if one forgets (or crashes mid-test),
     * clean up here so the sanitizer doesn't report leaks. */
    while (E.headbuf) {
        struct buffer *next = E.headbuf->next;
        destroyBuffer(E.headbuf);
        E.headbuf = next;
    }
    E.buf = NULL;
    E.headbuf = NULL;

    /* Free histories */
    freeHistory(&E.file_history);
    freeHistory(&E.command_history);
    freeHistory(&E.shell_history);
    freeHistory(&E.search_history);
    freeHistory(&E.kill_history);

    /* Free the kill text */
    clearText(&E.kill);

    /* Free registers */
    for (int r = 0; r < 127; r++) {
        if (E.registers[r].rtype == REGISTER_TEXT)
            clearText(&E.registers[r].data.text);
        E.registers[r].rtype = REGISTER_NULL;
    }

    /* Free macro */
    free(E.macro.keys);
    E.macro.keys = NULL;
    E.macro.nkeys = 0;
    E.macro.skeys = 0;

    /* Free render buffer */
    abFree(&E.render_buf);
    E.render_buf = (struct abuf)ABUF_INIT;

    /* Free windows */
    if (E.windows) {
        for (int i = 0; i < E.nwindows; i++)
            free(E.windows[i]);
        free(E.windows);
        E.windows = NULL;
    }

    /* Reset all remaining scalar state */
    E.nwindows = 0;
    E.recording = 0;
    E.playback = 0;
    E.micro = 0;
    E.uarg = 0;
    E.kill_ring_pos = 0;
    E.self_insert_key = 0;
    E.macro_depth = 0;
    E.lastVisitedBuffer = NULL;
    E.edbuf = NULL;
    E.minibuf = NULL;
    E.statusmsg[0] = '\0';
    E.statusmsg_show = 0;
    E.prefix_display[0] = '\0';
}


static void initTestEditor(void) {
    /* Always allocate fresh windows */
    E.windows = malloc(sizeof(struct window *));
    E.windows[0] = calloc(1, sizeof(struct window));

    E.screencols = 80;
    E.screenrows = 24;
    E.nwindows = 1;
    E.windows[0]->focused = 1;

    /* Initialize histories (safe to call on already-zeroed structs) */
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
