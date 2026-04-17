#include "message.h"
#include "util.h"

#ifndef EMIL_DISABLE_SHELL

/* Feature test macros must precede all system headers so that
 * fileno, fdopen, kill etc. are declared on strict platforms
 * (OpenIndiana / Solaris). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifdef __sun
#ifndef __EXTENSIONS__
#define __EXTENSIONS__ 1
#endif
#endif

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "buffer.h"
#include "dbuf.h"
#include "display.h"
#include "emil.h"
#include "fileio.h"
#include "pipe.h"
#include "prompt.h"
#include "region.h"
#include "emil_subprocess.h"
#include "unicode.h"
#include "util.h"
#include <errno.h>

extern struct config E;

static uint8_t *cmd;

static uint8_t *transformerPipeCmd(uint8_t *input) {
	/* Using sh -c lets us use pipes and stuff and takes care of quoting. */
	const char *command_line[4] = { "/bin/sh", "-c", (char *)cmd, NULL };
	struct subprocess_s subprocess;
	int result = subprocess_create(command_line,
				       subprocess_option_inherit_environment,
				       &subprocess);
	if (result) {
		setStatusMessage(
			"Shell command failed: unable to create subprocess");
		return NULL;
	}
	FILE *p_stdin = subprocess_stdin(&subprocess);
	FILE *p_stdout = subprocess_stdout(&subprocess);

	/* If we have a region send to subprocess */
	if (input) {
		for (int i = 0; input[i]; i++) {
			fputc(input[i], p_stdin);
		}
	}

	/* Join process */
	int sub_ret;
	if (subprocess_join(&subprocess, &sub_ret) != 0) {
		setStatusMessage(
			"Shell command failed: error waiting for subprocess");
		subprocess_destroy(&subprocess);
		return NULL;
	}

	/* Check if subprocess exited with error */
	if (sub_ret != 0) {
		setStatusMessage(msg_shell_exit_status, sub_ret);
		/* Continue anyway to show any output/errors */
	}

	/* Read stdout of process into buffer */
	struct dbuf d = DBUF_INIT;
	int c = fgetc(p_stdout);
	while (c != EOF) {
		dbuf_byte(&d, (uint8_t)c);
		c = fgetc(p_stdout);
	}

	/* Only show byte count if subprocess succeeded */
	if (sub_ret == 0) {
		setStatusMessage(msg_shell_read_bytes, d.len);
	}

	/* Cleanup & return — caller frees result */
	subprocess_destroy(&subprocess);
	return dbuf_detach(&d, NULL);
}

uint8_t *editorPipe(int useRegion) {
	cmd = NULL;
	cmd = editorPrompt(E.buf, (uint8_t *)"Shell: %s", PROMPT_BASIC, NULL);

	if (cmd == NULL) {
		setStatusMessage(msg_shell_canceled);
	} else if (useRegion) {
		if (E.uarg) {
			E.uarg = 0;
			transformRegion(transformerPipeCmd);
			// unmark region
			E.buf->markx = -1;
			E.buf->marky = -1;
			free(cmd);
			return NULL;
		} else {
			// 1. Extract the selected region
			if (markInvalid()) {
				setStatusMessage(msg_mark_invalid);
				free(cmd);
				return NULL;
			}

			copyRegion(); // E.kill.str now holds the selected text

			// 2. Pass the extracted text to transformerPipeCmd
			uint8_t *result = transformerPipeCmd(E.kill.str);

			free(cmd);
			return result;
		}
	} else {
		uint8_t *result = transformerPipeCmd(NULL);
		free(cmd);
		return result;
	}

	free(cmd);
	return NULL;
}

void pipeCmd(int useRegion) {
	/* Not allowed during macro record/playback */
	if (E.recording || E.playback) {
		setStatusMessage(msg_macro_blocked);
		return;
	}
	uint8_t *pipeOutput = editorPipe(useRegion);
	if (pipeOutput != NULL) {
		size_t outputLen = strlen((char *)pipeOutput);

		/* Validate UTF-8 before inserting into a buffer */
		if (!utf8_validate(pipeOutput, (int)outputLen)) {
			setStatusMessage("Shell output contains invalid UTF-8");
			free(pipeOutput);
			return;
		}

		struct buffer *shellBuf =
			findOrCreateSpecialBuffer("*Shell Output*");

		/* Singleton buffer — clear any content left over from a
		 * previous shell invocation and reset cursor/mark so stale
		 * positions don't dangle past the new content. */
		clearBuffer(shellBuf);
		shellBuf->cx = 0;
		shellBuf->cy = 0;
		shellBuf->markx = -1;
		shellBuf->marky = -1;
		shellBuf->mark_active = 0;

		// Use a temporary buffer to build each row
		size_t rowStart = 0;
		size_t rowLen = 0;

		for (size_t i = 0; i < outputLen; i++) {
			if (pipeOutput[i] == '\n') {
				insertRow(shellBuf, shellBuf->numrows,
					  (char *)&pipeOutput[rowStart],
					  rowLen);
				rowStart = i + 1;
				rowLen = 0;
			} else {
				rowLen++;
				if (i == outputLen - 1) {
					insertRow(shellBuf, shellBuf->numrows,
						  (char *)&pipeOutput[rowStart],
						  rowLen);
				}
			}
		}

		updateBuffer(shellBuf);

		/* Route the shell output to a window.  If a window is
		 * already showing *Shell Output*, move focus there instead
		 * of hijacking the current window.  Otherwise, swap the
		 * current window's buffer to shellBuf (original behaviour). */
		int existing = findBufferWindow(shellBuf);
		if (existing >= 0) {
			int curIdx = windowFocusedIdx();
			if (existing != curIdx) {
				/* Save the current window's view state
				 * before moving focus. */
				struct window *cur = E.windows[curIdx];
				cur->cx = cur->buf->cx;
				cur->cy = cur->buf->cy;
				cur->focused = 0;
				E.windows[existing]->focused = 1;
			}
			E.buf = shellBuf;
			synchronizeBufferCursor(shellBuf, E.windows[existing]);
		} else {
			int idx = windowFocusedIdx();
			E.windows[idx]->buf = shellBuf;
			E.buf = shellBuf;
		}
		refreshScreen();

		free(pipeOutput);
	}
}

/////
void diffBufferWithFile(void) {
	struct buffer *bufr = E.buf;
	if (bufr->filename == NULL) {
		setStatusMessage(msg_buffer_without_file);
		return;
	}

	if (!bufr->dirty) {
		setStatusMessage(msg_diff_buffer_matches_file);
		return;
	}

	/* Write the current buffer contents to a temp file.
	 * Use TMPDIR/TMP/TEMP for portability (MSYS2, etc). */
	const char *td = getenv("TMPDIR");
	if (!td || !*td)
		td = getenv("TMP");
	if (!td || !*td)
		td = getenv("TEMP");
	if (!td || !*td)
		td = "/tmp";
	size_t tdlen = strlen(td);
	/* td + "/emil-diff-XXXXXX" + NUL */
	char *tmpname = xmalloc(tdlen + 18);
	memcpy(tmpname, td, tdlen);
	memcpy(tmpname + tdlen, "/emil-diff-XXXXXX", 18); /* includes NUL */
	int fd = mkstemp(tmpname);
	if (fd == -1) {
		free(tmpname);
		setStatusMessage(msg_diff_cannot_create_temp);
		return;
	}

	size_t buflen;
	char *bufstr = rowsToString(bufr, &buflen);
	size_t total = 0;
	while (total < buflen) {
		ssize_t n = write(fd, bufstr + total, buflen - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmpname);
			free(tmpname);
			free(bufstr);
			setStatusMessage(msg_diff_cannot_write);
			return;
		}
		total += n;
	}
	close(fd);
	free(bufstr);

	/* Run diff directly, no shell — avoids filename quoting issues */
	char *iopath = expandTilde(bufr->filename);
	const char *command_line[5] = { "diff", "-u", iopath, tmpname, NULL };
	struct subprocess_s subprocess;
	int result =
		subprocess_create(command_line,
				  subprocess_option_inherit_environment |
					  subprocess_option_search_user_path,
				  &subprocess);
	if (result) {
		unlink(tmpname);
		free(tmpname);
		free(iopath);
		setStatusMessage(msg_diff_cannot_subprocess);
		return;
	}
	free(iopath);

	/* diff doesn't need stdin; just read stdout */
	int sub_ret = -1;
	subprocess_join(&subprocess, &sub_ret);

	FILE *p_stdout = subprocess_stdout(&subprocess);
	struct dbuf d = DBUF_INIT;
	int c = fgetc(p_stdout);
	while (c != EOF) {
		dbuf_byte(&d, (uint8_t)c);
		c = fgetc(p_stdout);
	}
	int output_len;
	char *output = (char *)dbuf_detach(&d, &output_len);

	subprocess_destroy(&subprocess);
	unlink(tmpname);
	free(tmpname);

	/* diff returns 0 = identical, 1 = differences, 2 = error */
	if (sub_ret == 0) {
		free(output);
		setStatusMessage(msg_diff_no_differences);
		return;
	}

	if (sub_ret >= 2 || output_len == 0) {
		free(output);
		setStatusMessage(msg_diff_failed, sub_ret);
		return;
	}

	struct buffer *diffBuf = findOrCreateSpecialBuffer("*Diff*");

	/* Singleton buffer — clear any content left over from a
	 * previous diff and reset cursor/mark so stale positions
	 * don't dangle past the new content. */
	clearBuffer(diffBuf);
	diffBuf->cx = 0;
	diffBuf->cy = 0;
	diffBuf->markx = -1;
	diffBuf->marky = -1;
	diffBuf->mark_active = 0;
	diffBuf->read_only = 1;

	size_t rowStart = 0;
	size_t rowLen = 0;
	size_t outLen = (size_t)output_len;
	for (size_t j = 0; j < outLen; j++) {
		if (output[j] == '\n' || j == outLen - 1) {
			insertRow(diffBuf, diffBuf->numrows, &output[rowStart],
				  rowLen);
			rowStart = j + 1;
			rowLen = 0;
		} else {
			rowLen++;
		}
	}

	updateBuffer(diffBuf);

	/* Route the diff output to a window.  If a window is already
	 * showing *Diff*, move focus there instead of hijacking the
	 * current window.  Otherwise, swap the current window's
	 * buffer to diffBuf. */
	int existing = findBufferWindow(diffBuf);
	if (existing >= 0) {
		int curIdx = windowFocusedIdx();
		if (existing != curIdx) {
			struct window *cur = E.windows[curIdx];
			cur->cx = cur->buf->cx;
			cur->cy = cur->buf->cy;
			cur->focused = 0;
			E.windows[existing]->focused = 1;
		}
		E.buf = diffBuf;
		synchronizeBufferCursor(diffBuf, E.windows[existing]);
	} else {
		int idx = windowFocusedIdx();
		E.windows[idx]->buf = diffBuf;
		E.buf = diffBuf;
	}
	refreshScreen();

	free(output);
}

#else /* EMIL_DISABLE_SHELL */

#include "display.h"
#include "emil.h"

void pipeCmd(int useRegion) {
	(void)useRegion; /* unused parameter */
	setStatusMessage(msg_shell_disabled);
}

void diffBufferWithFile(void) {
	setStatusMessage(msg_shell_disabled);
}

#endif /* EMIL_DISABLE_SHELL */
