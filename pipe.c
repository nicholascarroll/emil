#include "util.h"
#include "message.h"

#ifndef EMIL_DISABLE_SHELL

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* Define feature test macros for subprocess.h to get fdopen, fileno, kill */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif

#include "emil.h"
#include "region.h"
#include "pipe.h"
#include "subprocess.h"
#include "display.h"
#include "prompt.h"
#include "buffer.h"
#include "unicode.h"
#include "util.h"
#include "fileio.h"
#include <errno.h>

extern struct editorConfig E;

static uint8_t *cmd;
static char *buf;

static uint8_t *transformerPipeCmd(uint8_t *input) {
	int bsiz = BUFSIZ + 1;
	/* Using sh -c lets us use pipes and stuff and takes care of quoting. */
	const char *command_line[4] = { "/bin/sh", "-c", (char *)cmd, NULL };
	struct subprocess_s subprocess;
	int result = subprocess_create(command_line,
				       subprocess_option_inherit_environment,
				       &subprocess);
	if (result) {
		editorSetStatusMessage(
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
		editorSetStatusMessage(
			"Shell command failed: error waiting for subprocess");
		return NULL;
	}

	/* Check if subprocess exited with error */
	if (sub_ret != 0) {
		editorSetStatusMessage(msg_shell_exit_status, sub_ret);
		/* Continue anyway to show any output/errors */
	}

	/* Read stdout of process into buffer */
	int c = fgetc(p_stdout);
	int i = 0;
	while (c != EOF) {
		buf[i++] = c;
		buf[i] = 0;
		if (i >= bsiz - 10) {
			bsiz <<= 1;
			char *newbuf = xrealloc(buf, bsiz);
			buf = newbuf;
		}
		c = fgetc(p_stdout);
	}

	/* Only show byte count if subprocess succeeded */
	if (sub_ret == 0) {
		editorSetStatusMessage(msg_shell_read_bytes, i);
	}

	/* Cleanup & return */
	subprocess_destroy(&subprocess);
	return (uint8_t *)buf;
}

uint8_t *editorPipe(struct editorConfig *ed, struct editorBuffer *bf,
		    int useRegion) {
	buf = xcalloc(1, BUFSIZ + 1);
	cmd = NULL;
	cmd = editorPrompt(bf, (uint8_t *)"Shell: %s", PROMPT_BASIC, NULL);

	if (cmd == NULL) {
		editorSetStatusMessage(msg_shell_canceled);
	} else if (useRegion) {
		if (E.uarg) {
			E.uarg = 0;
			editorTransformRegion(ed, bf, transformerPipeCmd);
			// unmark region
			bf->markx = -1;
			bf->marky = -1;
			free(cmd);
			return NULL;
		} else {
			// 1. Extract the selected region
			if (markInvalid()) {
				editorSetStatusMessage(msg_mark_invalid);
				free(cmd);
				free(buf);
				return NULL;
			}

			editorCopyRegion(
				ed,
				bf); // ed->kill.str now holds the selected text

			// 2. Pass the extracted text to transformerPipeCmd
			uint8_t *result = transformerPipeCmd(ed->kill.str);

			free(cmd);
			return result;
		}
	} else {
		uint8_t *result = transformerPipeCmd(NULL);
		free(cmd);
		return result;
	}

	free(cmd);
	free(buf);
	return NULL;
}

void editorPipeCmd(struct editorConfig *ed, struct editorBuffer *bufr,
		   int useRegion) {
	uint8_t *pipeOutput = editorPipe(ed, bufr, useRegion);
	if (pipeOutput != NULL) {
		size_t outputLen = strlen((char *)pipeOutput);

		/* Validate UTF-8 before inserting into a buffer */
		if (!utf8_validate(pipeOutput, (int)outputLen)) {
			editorSetStatusMessage(
				"Shell output contains invalid UTF-8");
			free(pipeOutput);
			return;
		}

		if (outputLen < sizeof(ed->statusmsg) - 1) {
			editorSetStatusMessage("%s", pipeOutput);
		} else {
			struct editorBuffer *newBuf = newBuffer();
			newBuf->filename = xstrdup("*Shell Output*");
			newBuf->special_buffer = 1;

			// Use a temporary buffer to build each row
			size_t rowStart = 0;
			size_t rowLen = 0;
			for (size_t i = 0; i < outputLen; i++) {
				if (pipeOutput[i] == '\n' ||
				    i == outputLen - 1) {
					// Found a newline or end of output, insert the row
					editorInsertRow(
						newBuf, newBuf->numrows,
						(char *)&pipeOutput[rowStart],
						rowLen);
					rowStart =
						i + 1; // Start of the next row
					rowLen = 0;    // Reset row length
				} else {
					rowLen++;
				}
			}

			// Link the new buffer and update focus
			if (ed->headbuf == NULL) {
				ed->headbuf = newBuf;
			} else {
				struct editorBuffer *temp = ed->headbuf;
				while (temp->next != NULL) {
					temp = temp->next;
				}
				temp->next = newBuf;
			}
			ed->buf = newBuf;

			// Update the focused window
			int idx = windowFocusedIdx();
			ed->windows[idx]->buf = ed->buf;
			refreshScreen();
		}
		free(pipeOutput);
	}
}

/////
void editorDiffBufferWithFile(struct editorConfig *ed,
			      struct editorBuffer *bufr) {
	if (bufr->filename == NULL) {
		editorSetStatusMessage(msg_buffer_without_file);
		return;
	}

	if (!bufr->dirty) {
		editorSetStatusMessage(msg_diff_buffer_matches_file);
		return;
	}

	/* Write the current buffer contents to a temp file */
	char tmpname[] = "/tmp/emil-diff-XXXXXX";
	int fd = mkstemp(tmpname);
	if (fd == -1) {
		editorSetStatusMessage(msg_diff_cannot_create_temp);
		return;
	}

	int buflen;
	char *bufstr = editorRowsToString(bufr, &buflen);
	ssize_t total = 0;
	while (total < buflen) {
		ssize_t n = write(fd, bufstr + total, buflen - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmpname);
			free(bufstr);
			editorSetStatusMessage(msg_diff_cannot_write);
			return;
		}
		total += n;
	}
	close(fd);
	free(bufstr);

	/* Run diff directly, no shell — avoids filename quoting issues */
	const char *command_line[5] = { "diff", "-u", bufr->filename, tmpname,
					NULL };
	struct subprocess_s subprocess;
	int result =
		subprocess_create(command_line,
				  subprocess_option_inherit_environment |
					  subprocess_option_search_user_path,
				  &subprocess);
	if (result) {
		unlink(tmpname);
		editorSetStatusMessage(msg_diff_cannot_subprocess);
		return;
	}

	/* diff doesn't need stdin; just read stdout */
	int sub_ret;
	subprocess_join(&subprocess, &sub_ret);

	FILE *p_stdout = subprocess_stdout(&subprocess);
	int bsiz = BUFSIZ + 1;
	char *output = xcalloc(1, bsiz);
	int c = fgetc(p_stdout);
	int i = 0;
	while (c != EOF) {
		output[i++] = c;
		output[i] = 0;
		if (i >= bsiz - 10) {
			bsiz <<= 1;
			output = xrealloc(output, bsiz);
		}
		c = fgetc(p_stdout);
	}

	subprocess_destroy(&subprocess);
	unlink(tmpname);

	/* diff returns 0 = identical, 1 = differences, 2 = error */
	if (sub_ret == 0) {
		free(output);
		editorSetStatusMessage(msg_diff_no_differences);
		return;
	}

	if (sub_ret >= 2 || i == 0) {
		free(output);
		editorSetStatusMessage(msg_diff_failed, sub_ret);
		return;
	}

	/* Create a *Diff* buffer with the output */
	struct editorBuffer *diffBuf = newBuffer();
	diffBuf->filename = xstrdup("*Diff*");
	diffBuf->special_buffer = 1;
	diffBuf->read_only = 1;

	size_t rowStart = 0;
	size_t rowLen = 0;
	size_t outputLen = (size_t)i;
	for (size_t j = 0; j < outputLen; j++) {
		if (output[j] == '\n' || j == outputLen - 1) {
			editorInsertRow(diffBuf, diffBuf->numrows,
					&output[rowStart], rowLen);
			rowStart = j + 1;
			rowLen = 0;
		} else {
			rowLen++;
		}
	}

	/* Link the new buffer into the buffer list */
	if (ed->headbuf == NULL) {
		ed->headbuf = diffBuf;
	} else {
		struct editorBuffer *temp = ed->headbuf;
		while (temp->next != NULL) {
			temp = temp->next;
		}
		temp->next = diffBuf;
	}
	ed->buf = diffBuf;

	int idx = windowFocusedIdx();
	ed->windows[idx]->buf = ed->buf;
	refreshScreen();

	free(output);
}

#else /* EMIL_DISABLE_SHELL */

#include "emil.h"
#include "display.h"

void editorPipeCmd(struct editorConfig *ed, struct editorBuffer *bufr) {
	(void)ed;   /* unused parameter */
	(void)bufr; /* unused parameter */
	editorSetStatusMessage(msg_shell_disabled);
}

void editorDiffBufferWithFile(struct editorConfig *ed,
			      struct editorBuffer *bufr) {
	(void)ed;
	(void)bufr;
	editorSetStatusMessage(msg_shell_disabled);
}

#endif /* EMIL_DISABLE_SHELL */
