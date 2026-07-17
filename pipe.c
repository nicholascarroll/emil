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
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
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

/* Interrupt source watched during a running command.  The editor
 * uses the terminal (STDIN_FILENO); tests substitute their own
 * pipe via pipeCommandCaptureIntr, and -1 disables interruption. */
static int pipe_intr_fd = STDIN_FILENO;

/* 1 if the most recent transformerPipeCmd run was cancelled (as
 * opposed to failing).  Read by pipeCommandCaptureIntr. */
static int pipe_last_canceled;

/* Pump 'input' into sp's stdin while draining its stdout into 'out'
 * and discarding its stderr, using select() so neither side can
 * deadlock on a full pipe (~64 KB).  Closes and NULLs
 * sp->stdin_file once input is exhausted so the child sees EOF (and
 * join/destroy don't close it again).  'input' may be NULL for
 * commands that take no stdin.
 *
 * If intr_fd >= 0 it is watched for C-g (0x07); any other byte is
 * consumed and discarded (type-ahead during a synchronous command
 * is dropped, as in Emacs).  Escalation is user-driven, as in
 * Emacs — no timers of any kind in this path:
 *
 *   1st C-g: SIGINT to the child's process group, stop feeding
 *            stdin, keep draining until the pipes reach EOF.
 *   2nd C-g: SIGKILL to the group (for children that ignore or
 *            trap SIGINT).
 *   3rd C-g: abandon the pipes entirely (a child unkillable even
 *            by SIGKILL is in uninterruptible sleep on a dead
 *            filesystem; the editor must not be wedged by it).
 *
 * Returns 0 if the command ran to completion, nonzero (the stage
 * reached) if cancelled. */
static int pumpSubprocessIO(struct subprocess_s *sp, uint8_t *input,
			    struct dbuf *out, int intr_fd) {
	int in_fd = sp->stdin_file ? fileno(sp->stdin_file) : -1;
	int out_fd = sp->stdout_file ? fileno(sp->stdout_file) : -1;
	int err_fd = sp->stderr_file ? fileno(sp->stderr_file) : -1;

	size_t in_len = input ? strlen((char *)input) : 0;
	size_t in_off = 0;

	if (in_fd >= 0)
		fcntl(in_fd, F_SETFL, O_NONBLOCK);

	int out_open = (out_fd >= 0);
	int err_open = (err_fd >= 0);
	int cancel_stage = 0;

	while (out_open || err_open || (cancel_stage == 0 && in_off < in_len)) {
		/* Nothing left to write (or cancelled): close stdin
		 * so the child sees EOF. */
		if (sp->stdin_file && (cancel_stage > 0 || in_off >= in_len)) {
			fclose(sp->stdin_file);
			sp->stdin_file = NULL;
			in_fd = -1;
		}

		fd_set rfds, wfds;
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);
		int maxfd = -1;
		if (out_open) {
			FD_SET(out_fd, &rfds);
			if (out_fd > maxfd)
				maxfd = out_fd;
		}
		if (err_open) {
			FD_SET(err_fd, &rfds);
			if (err_fd > maxfd)
				maxfd = err_fd;
		}
		if (intr_fd >= 0) {
			FD_SET(intr_fd, &rfds);
			if (intr_fd > maxfd)
				maxfd = intr_fd;
		}
		int want_write =
			(cancel_stage == 0 && in_fd >= 0 && in_off < in_len);
		if (want_write) {
			FD_SET(in_fd, &wfds);
			if (in_fd > maxfd)
				maxfd = in_fd;
		}
		if (maxfd < 0)
			break;

		if (select(maxfd + 1, &rfds, want_write ? &wfds : NULL, NULL,
			   NULL) < 0) {
			if (errno == EINTR)
				continue;
			break;
		}

		uint8_t io[4096];
		if (intr_fd >= 0 && FD_ISSET(intr_fd, &rfds)) {
			ssize_t n = read(intr_fd, io, sizeof(io));
			if (n < 0 && (errno == EINTR || errno == EAGAIN)) {
				; /* retry next iteration */
			} else if (n <= 0) {
				intr_fd = -1; /* source gone: stop watching */
			} else {
				for (ssize_t i = 0; i < n; i++) {
					if (io[i] != 0x07) /* not C-g */
						continue;
					cancel_stage++;
					if (cancel_stage == 1) {
						subprocess_signal(sp, SIGINT);
						if (intr_fd == STDIN_FILENO) {
							setStatusMessage(
								msg_shell_interrupted);
							refreshScreen();
						}
					} else if (cancel_stage == 2) {
						subprocess_signal(sp, SIGKILL);
					}
				}
				if (cancel_stage >= 3)
					break; /* abandon unkillable child */
			}
		}
		if (out_open && FD_ISSET(out_fd, &rfds)) {
			ssize_t n = read(out_fd, io, sizeof(io));
			if (n > 0)
				dbuf_append(out, io, (int)n);
			else if (n < 0 && (errno == EINTR || errno == EAGAIN))
				; /* interrupted: retry next iteration */
			else
				out_open = 0;
		}
		if (err_open && FD_ISSET(err_fd, &rfds)) {
			/* Drain and discard: a chatty child must not
			 * block on a full stderr pipe either. */
			ssize_t n = read(err_fd, io, sizeof(io));
			if (n < 0 && (errno == EINTR || errno == EAGAIN))
				; /* interrupted: retry next iteration */
			else if (n <= 0)
				err_open = 0;
		}
		if (want_write && FD_ISSET(in_fd, &wfds)) {
			ssize_t n =
				write(in_fd, input + in_off, in_len - in_off);
			if (n > 0)
				in_off += (size_t)n;
			else if (n < 0 && errno != EAGAIN && errno != EINTR)
				in_off = in_len; /* child closed stdin
						  * (SIGPIPE is ignored) */
		}
	}

	if (sp->stdin_file) {
		fclose(sp->stdin_file);
		sp->stdin_file = NULL;
	}
	return cancel_stage;
}

static uint8_t *transformerPipeCmd(uint8_t *input) {
	pipe_last_canceled = 0;
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
	/* Pump region text in and command output out concurrently;
	 * see pumpSubprocessIO for why this must be interleaved.
	 * pipe_intr_fd (the terminal in the editor) is watched so C-g
	 * cancels a long-running command without losing the session. */
	struct dbuf d = DBUF_INIT;
	int canceled =
		(pumpSubprocessIO(&subprocess, input, &d, pipe_intr_fd) != 0);

	pipe_last_canceled = canceled;
	if (canceled) {
		/* Reap without ever blocking: the child has had SIGTERM
		 * and possibly SIGKILL; if it is unreapable even now
		 * (D-state on a hung filesystem), abandon it rather
		 * than wedge the editor in waitpid — the one zombie is
		 * the lesser evil.  select() doubles as a portable
		 * sub-second sleep. */
		for (int i = 0; i < 20; i++) {
			if (subprocess_tryjoin(&subprocess, NULL) != 0)
				break;
			struct timeval nap = { 0, 100000 };
			select(0, NULL, NULL, NULL, &nap);
		}
		subprocess_destroy(&subprocess);
		dbuf_free(&d);
		setStatusMessage(msg_canceled);
		return NULL;
	}

	/* Join process */
	int sub_ret;
	if (subprocess_join(&subprocess, &sub_ret) != 0) {
		setStatusMessage(
			"Shell command failed: error waiting for subprocess");
		subprocess_destroy(&subprocess);
		dbuf_free(&d);
		return NULL;
	}

	/* Check if subprocess exited with error */
	if (sub_ret != 0) {
		setStatusMessage(msg_shell_exit_status, sub_ret);
		/* Continue anyway to show any output/errors */
	}

	/* Only show byte count if subprocess succeeded */
	if (sub_ret == 0) {
		setStatusMessage(msg_shell_read_bytes, d.len);
	}

	/* Cleanup & return — caller frees result */
	subprocess_destroy(&subprocess);
	return dbuf_detach(&d, NULL);
}

/* Run 'command' through /bin/sh -c with optional 'input' on stdin,
 * returning captured stdout (caller frees, NULL on spawn failure).
 * Thin wrapper over the static transformerPipeCmd so tests can
 * exercise the real subprocess I/O path instead of replicating it. */
uint8_t *pipeCommandCapture(const uint8_t *command, uint8_t *input) {
	return pipeCommandCaptureIntr(command, input, -1, NULL);
}

/* As pipeCommandCapture, but watching intr_fd for C-g cancellation.
 * *out_canceled (if non-NULL) is set to 1 when the command was
 * cancelled, 0 otherwise. */
uint8_t *pipeCommandCaptureIntr(const uint8_t *command, uint8_t *input,
				int intr_fd, int *out_canceled) {
	int saved_intr = pipe_intr_fd;
	pipe_intr_fd = intr_fd;
	cmd = (uint8_t *)command;
	uint8_t *out = transformerPipeCmd(input);
	cmd = NULL;
	pipe_intr_fd = saved_intr;
	if (out_canceled)
		*out_canceled = pipe_last_canceled;
	return out;
}

uint8_t *editorPipe(int useRegion) {
	cmd = NULL;
	cmd = editorPrompt(E.buf, "Shell: %s", PROMPT_BASIC, NULL);

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
					  &pipeOutput[rowStart], rowLen);
				rowStart = i + 1;
				rowLen = 0;
			} else {
				rowLen++;
				if (i == outputLen - 1) {
					insertRow(shellBuf, shellBuf->numrows,
						  &pipeOutput[rowStart],
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

	/* diff takes no stdin, but its stdout must be drained BEFORE
	 * joining: a diff larger than the pipe capacity (~64 KB) blocks
	 * the child on write, and waitpid never returns. */
	struct dbuf d = DBUF_INIT;
	pumpSubprocessIO(&subprocess, NULL, &d, -1);

	int sub_ret = -1;
	subprocess_join(&subprocess, &sub_ret);

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
			insertRow(diffBuf, diffBuf->numrows,
				  (const uint8_t *)&output[rowStart], rowLen);
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

uint8_t *pipeCommandCapture(const uint8_t *command, uint8_t *input) {
	(void)command;
	(void)input;
	setStatusMessage(msg_shell_disabled);
	return NULL;
}

uint8_t *pipeCommandCaptureIntr(const uint8_t *command, uint8_t *input,
				int intr_fd, int *out_canceled) {
	(void)command;
	(void)input;
	(void)intr_fd;
	if (out_canceled)
		*out_canceled = 0;
	setStatusMessage(msg_shell_disabled);
	return NULL;
}

#endif /* EMIL_DISABLE_SHELL */
