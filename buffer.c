/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "emil.h"

#include "buffer.h"
#include "fileio.h"
#include "unicode.h"
#include "undo.h"
#include "prompt.h"
#include "display.h"
#include "util.h"
#include "terminal.h"
#include "window.h"
#include "wrap.h"
#include <limits.h>

int rejectIfReadOnly(struct buffer *buf) {
	if (buf->read_only) {
		setStatusMessage("Buffer is read-only");
		return 1;
	}
	return 0;
}

/* Dirty-state transitions.  Editing the content mutates many places,
 * so they all route through these two helpers rather than touching
 * buf->dirty directly.  The advisory file lock tracks the dirty
 * state: we hold the lock while the buffer has unsaved changes, and
 * release it the moment the buffer matches what's on disk.
 *
 * Special / nameless  buffer  never get locked.  Lock-acquisition
 * failures during editing are tolerated: we honour the user's edit
 * (the buffer is already modified by the time we're called) and
 * leave whatever status message lockFile posted in place. */

void markBufferDirty(struct buffer *buf) {
	if (buf->dirty)
		return;
	buf->dirty = 1;
	if (buf->filename == NULL || buf->special_buffer || buf->read_only)
		return;
	if (buf->lock_fd >= 0)
		return; /* already locked (e.g. from previous session) */
	if (buf->external_mod)
		return; /* buffer no longer reflects on-disk content
		         * User must revert or save-as to resolve. */
	char *iopath = expandTilde(buf->filename);
	if (lockFile(buf, iopath) == 0) {
		/* Lock acquired: clear any prior "blocked by PID" state. */
		buf->lock_blocked_pid = 0;
	}
	free(iopath);
}

void markBufferClean(struct buffer *buf) {
	if (!buf->dirty)
		return;
	buf->dirty = 0;
	if (buf->lock_fd >= 0)
		releaseLock(buf);
	/* If a previous acquire attempt set lock_blocked_pid, clear it
	 * now: once clean, we aren't trying to hold the lock, so the
	 * warning isn't meaningful.  The next edit will re-probe. */
	buf->lock_blocked_pid = 0;
}

/* Grow row->chars so it can hold at least `needed` bytes.  Doubling
 * from a floor of 16, jumping straight to `needed` when that is larger.
 */
void rowEnsureCap(erow *row, int needed) {
	if (needed <= row->charcap)
		return;
	/* A row is bounded by EMIL_MAX_FILE_SIZE (1 GiB) at load, but 
	 * nothing re-imposes that during editing, so cap the doubling
	 * instead of overflowing into a negative capacity, which would
	 * make the `new_cap < needed` line below silently allocate too
	 * little. */
	int new_cap = row->charcap < 16		 ? 16 :
		      row->charcap > INT_MAX / 2 ? INT_MAX :
						   row->charcap * 2;
	if (new_cap < needed)
		new_cap = needed;
	row->chars = xrealloc(row->chars, new_cap);
	row->charcap = new_cap;
}

/* Grow the buffer's row array by one, zeroing the new slots. */
static void bufEnsureRowCap(struct buffer *bufr) {
	if (bufr->numrows < bufr->rowcap)
		return;
	/* The row count is bounded at INT_MAX / 2 by the load path, so the
	 * cap below is reached before the multiplication can overflow.*/
	int new_cap = !bufr->rowcap		 ? 16 :
		      bufr->rowcap > INT_MAX / 2 ? INT_MAX :
						   bufr->rowcap * 2;
	bufr->row = xrealloc(bufr->row, sizeof(erow) * new_cap);
	memset(&bufr->row[bufr->rowcap], 0,
	       sizeof(erow) * (new_cap - bufr->rowcap));
	bufr->rowcap = new_cap;
}

void insertRow(struct buffer *bufr, int at, const uint8_t *s, size_t len) {
	if (at < 0 || at > bufr->numrows)
		return;

	bufEnsureRowCap(bufr);

	if (at < bufr->numrows) {
		memmove(&bufr->row[at + 1], &bufr->row[at],
			sizeof(erow) * (bufr->numrows - at));
	}

	bufr->row[at].size = len;
	bufr->row[at].chars = xmalloc(len + 1);
	bufr->row[at].charcap = len + 1;
	memcpy(bufr->row[at].chars, s, len);
	bufr->row[at].chars[len] = '\0';

	bufr->row[at].cached_width = -1;

	bufr->numrows++;
	markBufferDirty(bufr);
}

/* Append a row without side effects.  Used by `editorOpen` when the
 * buffer is being populated from disk.
 */
void appendRowRaw(struct buffer *bufr, const uint8_t *s, size_t len) {
	bufEnsureRowCap(bufr);

	int at = bufr->numrows;
	bufr->row[at].size = len;
	bufr->row[at].chars = xmalloc(len + 1);
	bufr->row[at].charcap = len + 1;
	memcpy(bufr->row[at].chars, s, len);
	bufr->row[at].chars[len] = '\0';
	bufr->row[at].cached_width = -1;

	bufr->numrows++;
}

void freeRow(erow *row) {
	free(row->chars);
}

void delRow(struct buffer *bufr, int at) {
	if (at < 0 || at >= bufr->numrows)
		return;
	freeRow(&bufr->row[at]);
	if (at == bufr->numrows - 1) {
		// Last row, no need to memmove
		bufr->numrows--;
	} else {
		memmove(&bufr->row[at], &bufr->row[at + 1],
			sizeof(erow) * (bufr->numrows - at - 1));
		bufr->numrows--;
	}
	markBufferDirty(bufr);
}

void rowInsertChar(struct buffer *bufr, erow *row, int at, int c) {
	if (at < 0 || at > row->size)
		at = row->size;

	int needed = row->size + 2;
	rowEnsureCap(row, needed);
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	markBufferDirty(bufr);
	row->cached_width = -1;
}

struct buffer *newBuffer(void) {
	struct buffer *ret = xmalloc(sizeof(struct buffer));
	ret->markx = -1;
	ret->marky = -1;
	ret->mark_active = 0;
	ret->mark_ring_len = 0;
	ret->mark_ring_idx = 0;
	for (int i = 0; i < MARK_RING_SIZE; i++) {
		ret->mark_ring[i].cx = -1;
		ret->mark_ring[i].cy = -1;
	}
	ret->cx = 0;
	ret->cy = 0;
	ret->numrows = 0;
	ret->rowcap = 0;
	ret->row = NULL;
	ret->filename = NULL;
	ret->display_name = NULL;
	ret->min_name_len = 0;
	ret->query = NULL;
	ret->match_len = 0;
	ret->dirty = 0;
	ret->special_buffer = 0;
	ret->undo = NULL;
	ret->redo = NULL;
	ret->completionState.last_completed_text = NULL;
	ret->completionState.completion_start_pos = 0;
	ret->completionState.successive_tabs = 0;
	ret->completionState.last_completion_count = 0;
	ret->completionState.preserve_message = 0;
	ret->completionState.selected = -1;
	ret->completionState.matches = NULL;
	ret->completionState.n_matches = 0;
	ret->next = NULL;
	ret->word_wrap = 0;
	ret->rectangle_mode = 0;
	ret->read_only = 0;
	ret->read_only_by_lock = 0;
	ret->lock_fd = -1;
	ret->open_mtime = 0;
	ret->open_size = 0;
	ret->external_mod = 0;
	ret->lock_blocked_pid = 0;
	ret->internal_mod = 0;

	/* Establish numrows >= 1. */
	appendRowRaw(ret, (const uint8_t *)"", 0);
	return ret;
}

/* Discard every row, leaving the buffer transiently rowless.  For the
 * loaders, which build a row array from scratch and restore the
 * invariant before returning; see the invariant note in buffer.h. */
/* Split a text blob into buffer rows, appending them (#117 R2).
 *
 * The one answer to "how does a blob of bytes become rows".  Five
 * places used to do this, and they disagreed in two ways the report
 * catalogued: only the stdin path stripped CR, and the *Diff* copy
 * dropped the final byte of output that did not end in a newline
 * (DEF-4) while its sibling 165 lines away in the same file got it
 * right.
 *
 * Appends rather than resets: register.c writes a header row first
 * and then the register's text beneath it.  Callers wanting a fresh
 * buffer call bufferResetRows() themselves, which is also where the
 * cursor/mark reset belongs.
 *
 * Rows are appended with appendRowRaw, so this does not mark the
 * buffer dirty or take a lock.  That is what a bulk population wants
 * — §3.21.1 requires it of the load path, so that an unedited file
 * is never rewritten merely because it was opened.
 *
 * BLOB_CRLF       strip one trailing '\r' from each line (DOS input).
 * BLOB_FINAL_NL   terminate with an empty final row unconditionally,
 *                 the representation of a trailing newline (§4.1).
 *                 Without it a blob not ending in '\n' yields a final
 *                 row holding those bytes, and one that does ends on
 *                 the last complete line.
 *
 * Guarantees at least one row on return (§4.9), so a caller need not
 * follow with bufferEnsureRow. */
void bufferLoadBlob(struct buffer *buf, const uint8_t *data, size_t len,
		    int flags) {
	size_t start = 0;

	for (size_t i = 0; i < len; i++) {
		if (data[i] != '\n')
			continue;
		size_t end = i;
		if ((flags & BLOB_CRLF) && end > start && data[end - 1] == '\r')
			end--;
		appendRowRaw(buf, data + start, end - start);
		start = i + 1;
	}

	/* Trailing bytes with no newline after them are a row too.
	 * Counting them here, rather than inside the loop on its last
	 * iteration, is what the *Diff* copy got wrong. */
	if (start < len) {
		size_t end = len;
		if ((flags & BLOB_CRLF) && end > start && data[end - 1] == '\r')
			end--;
		appendRowRaw(buf, data + start, end - start);
	}

	if (flags & BLOB_FINAL_NL)
		appendRowRaw(buf, (const uint8_t *)"", 0);

	bufferEnsureRow(buf);
}

void bufferResetRows(struct buffer *bufr) {
	for (int i = 0; i < bufr->numrows; i++)
		freeRow(&bufr->row[i]);
	free(bufr->row);
	bufr->row = NULL;
	bufr->numrows = 0;
	bufr->rowcap = 0;
}

/* The empty buffer is the one-row buffer whose single row is empty. */
int bufferIsEmpty(struct buffer *bufr) {
	return bufr->numrows == 1 && bufr->row[0].size == 0;
}

/* The number of lines of text, as a user counts them, which is not
 * numrows.  Under the representation a trailing newline is a final
 * empty row, so "a\nb\n" is three rows but two lines.  A file with no
 * trailing newline has no such row and its line count is numrows.
 *
 * For display only.  Line *numbering* does not go through here: point
 * may legitimately sit on the final empty row, so the cursor line
 * stays cy + 1 and can exceed this count by one.*/
int bufferLineCount(struct buffer *bufr) {
	if (bufr->numrows > 0 && bufr->row[bufr->numrows - 1].size == 0)
		return bufr->numrows - 1;
	return bufr->numrows;
}

void destroyBuffer(struct buffer *buf) {
	if (E.lastVisitedBuffer == buf)
		E.lastVisitedBuffer = NULL;
	releaseLock(buf);
	clearUndosAndRedos(buf);
	free(buf->filename);
	free(buf->display_name);
	free(buf->query);
	free(buf->completionState.last_completed_text);
	if (buf->completionState.matches) {
		for (int i = 0; i < buf->completionState.n_matches; i++)
			free(buf->completionState.matches[i]);
		free(buf->completionState.matches);
	}
	for (int i = 0; i < buf->numrows; i++) {
		freeRow(&buf->row[i]);
	}
	free(buf->row);
	free(buf);
}

void updateBuffer(struct buffer *buf) {
	for (int i = 0; i < buf->numrows; i++) {
		buf->row[i].cached_width = -1;
	}
}

struct buffer *findBufferByName(const char *name) {
	/* Literal match (fast path, covers special buffers) */
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
		if (b->filename && strcmp(b->filename, name) == 0)
			return b;
	}

	/* For real file paths, compare absolute forms to avoid
	 * duplicate buffers opened via different path forms. */
	if (name[0] == '*')
		return NULL;

	char *abs_name = absolutePath(name);
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
		if (!b->filename || b->special_buffer)
			continue;
		char *abs_buf = absolutePath(b->filename);
		int match = (strcmp(abs_buf, abs_name) == 0);
		free(abs_buf);
		if (match) {
			free(abs_name);
			return b;
		}
	}
	free(abs_name);
	return NULL;
}

struct buffer *findOrCreateSpecialBuffer(const char *name) {
	struct buffer *buf = findBufferByName(name);
	if (buf)
		return buf;
	buf = newBuffer();
	buf->filename = xstrdup(name);
	buf->special_buffer = 1;
	buf->read_only = 1;
	buf->next = E.headbuf;
	E.headbuf = buf;
	return buf;
}

/* Restore the invariant after bufferResetRows.  The counterpart to it:
 * reset, append whatever rows the content produces, then call this so
 * that content which produced no rows at all still leaves a valid
 * buffer rather than a rowless one. */
void bufferEnsureRow(struct buffer *buf) {
	if (buf->numrows == 0)
		appendRowRaw(buf, (const uint8_t *)"", 0);
}

void closeSpecialBuffer(const char *name) {
	struct buffer *target = NULL;
	struct buffer *prev = NULL;

	for (struct buffer *b = E.headbuf; b != NULL; prev = b, b = b->next) {
		if (b->filename && strcmp(b->filename, name) == 0) {
			target = b;
			break;
		}
	}
	if (!target)
		return;

	int win = findBufferWindow(target);
	if (win >= 0 && E.nwindows > 1)
		destroyWindow(win);

	if (prev)
		prev->next = target->next;
	else
		E.headbuf = target->next;

	if (E.buf == target)
		E.buf = target->next ? target->next : E.headbuf;

	destroyBuffer(target);
}

void switchToNamedBuffer(void) {
	char prompt[512];
	struct buffer *defaultBuffer = NULL;

	if (E.lastVisitedBuffer && E.lastVisitedBuffer != E.buf) {
		defaultBuffer = E.lastVisitedBuffer;
	} else {
		/* Find the first buffer that isn't the current one */
		struct buffer *b = E.headbuf;
		while (b == E.buf && b->next)
			b = b->next;
		if (b != E.buf)
			defaultBuffer = b;
	}

	if (defaultBuffer) {
		const char *full = defaultBuffer->filename ?
					   defaultBuffer->filename :
					   "*scratch*";
		const char *slash = strrchr(full, '/');
		const char *base = slash ? slash + 1 : full;
		/* Prompt is a plain prefix (see editorPrompt): the
		 * basename embeds verbatim, no percent escaping.
		 * %.255s keeps the prompt reasonable for absurdly
		 * long names; snprintf bounds it regardless. */
		snprintf(prompt, sizeof(prompt),
			 "Switch to buffer (default %.255s): ", base);
	} else {
		snprintf(prompt, sizeof(prompt), "Switch to buffer: ");
	}

	uint8_t *buffer_name = editorPrompt(E.buf, prompt, PROMPT_BUFFER, NULL);

	if (buffer_name == NULL) {
		setStatusMessage("Buffer switch canceled");
		return;
	}

	struct buffer *targetBuffer = NULL;

	if (buffer_name[0] == '\0') {
		/* User pressed Enter without typing */
		targetBuffer = defaultBuffer;
		if (!targetBuffer) {
			setStatusMessage("No buffer to switch to");
			free(buffer_name);
			return;
		}
	} else {
		/* Try exact match on full path first */
		for (struct buffer *buf = E.headbuf; buf != NULL;
		     buf = buf->next) {
			if (buf == E.buf)
				continue;
			const char *name = buf->filename ? buf->filename :
							   "*scratch*";
			if (strcmp((char *)buffer_name, name) == 0) {
				targetBuffer = buf;
				break;
			}
		}

		/* If no exact full-path match, try basename match */
		if (!targetBuffer) {
			struct buffer *basename_match = NULL;
			int match_count = 0;
			for (struct buffer *buf = E.headbuf; buf != NULL;
			     buf = buf->next) {
				if (buf == E.buf)
					continue;
				const char *name = buf->filename ?
							   buf->filename :
							   "*scratch*";
				const char *slash = strrchr(name, '/');
				const char *base = slash ? slash + 1 : name;
				if (strcmp((char *)buffer_name, base) == 0) {
					basename_match = buf;
					match_count++;
				}
			}
			if (match_count == 1) {
				targetBuffer = basename_match;
			} else if (match_count > 1) {
				setStatusMessage("[Complete, but not unique]");
				free(buffer_name);
				return;
			}
		}

		if (!targetBuffer) {
			setStatusMessage("No buffer named '%s'", buffer_name);
			free(buffer_name);
			return;
		}
	}

	E.lastVisitedBuffer = E.buf;
	E.buf = targetBuffer;
	resetFileCheckThrottle();

	const char *full = E.buf->filename ? E.buf->filename : "*scratch*";
	int n = snprintf(NULL, 0, "Switched to buffer %s", full);
	char *switchedName = leftTruncate(full, nameFit(full, n));
	setStatusMessage("Switched to buffer %s", switchedName);
	free(switchedName);

	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->focused) {
			E.windows[i]->buf = E.buf;
		}
	}

	free(buffer_name);
}

void previousBuffer(void) {
	E.buf = E.buf->next;
	if (E.buf == NULL) {
		E.buf = E.headbuf;
	}
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->focused) {
			E.windows[i]->buf = E.buf;
		}
	}
	resetFileCheckThrottle();
}

void nextBuffer(void) {
	if (E.buf == E.headbuf) {
		// If we're at the first buffer, go to the last buffer
		while (E.buf->next != NULL) {
			E.buf = E.buf->next;
		}
	} else {
		// Otherwise, go to the previous buffer
		struct buffer *temp = E.headbuf;
		while (temp->next != E.buf) {
			temp = temp->next;
		}
		E.buf = temp;
	}
	// Update the focused buffer in all windows
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->focused) {
			E.windows[i]->buf = E.buf;
		}
	}
	resetFileCheckThrottle();
}

/* Confirm only for a modified buffer that is visiting a file, matching
 * kill-buffer in Emacs.*/
int killBufferNeedsConfirm(const struct buffer *bufr) {
	return bufr->dirty && bufr->filename != NULL && !bufr->special_buffer;
}

void killBuffer(void) {
	struct buffer *bufr = E.buf;

	if (killBufferNeedsConfirm(bufr)) {
		const char *fname = bufr->filename;
		int n = snprintf(NULL, 0,
				 "Buffer %s modified; kill anyway? (y or n)",
				 fname);
		char *killName = leftTruncate(fname, nameFit(fname, n));
		setStatusMessage("Buffer %s modified; kill anyway? (y or n)",
				 killName);
		free(killName);
		refreshScreen();
		int c = readKey();
		clearStatusMessage();
		if (c != 'y' && c != 'Y') {
			return;
		}
	}

	// Find the previous buffer (if any)
	struct buffer *prevBuf = NULL;
	if (E.buf != E.headbuf) {
		prevBuf = E.headbuf;
		while (prevBuf->next != E.buf) {
			prevBuf = prevBuf->next;
		}
	}

	struct buffer *scratch = NULL;
	if (bufr->next == NULL && prevBuf == NULL) {
		scratch = newBuffer();
		scratch->filename = xstrdup("*scratch*");
		scratch->special_buffer = 1;
	}

	// Update window focus
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->buf == bufr) {
			// If it's the last buffer, use the shared scratch
			if (scratch != NULL) {
				E.windows[i]->buf = scratch;
				E.headbuf = scratch;
				E.buf = E.headbuf;
			} else if (bufr->next == NULL) {
				E.windows[i]->buf = E.headbuf;
				E.buf = E.headbuf;
			} else {
				E.windows[i]->buf = bufr->next;
				E.buf = bufr->next;
			}
		}
	}

	// Update the main buffer list
	if (E.headbuf == bufr) {
		E.headbuf = bufr->next;
	} else if (prevBuf != NULL) {
		prevBuf->next = bufr->next;
	}

	// Update the focused buffer
	if (E.buf == bufr) {
		E.buf = (bufr->next != NULL) ? bufr->next : prevBuf;
	}

	destroyBuffer(bufr);
	resetFileCheckThrottle();
	computeDisplayNames();
}

/* Basename helper: returns pointer into path after the last '/'. */
static const char *baseName(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* Left-truncate a string to fit in max_width display COLUMNS,
 * prepending "..." when there is room for it.  Returns a newly
 * allocated string.
 *
 * DEF-1 (#117): every caller passes a column budget, but this
 * function measured in bytes — `s + (len - tail)` indexed by byte
 * count and landed inside multi-byte sequences, leaking invalid
 * UTF-8 into display_name, the status bar, completion lists and
 * save/open/kill messages (the 0.9.3 CHANGELOG fix for this repaired
 * only the other copy of the loop, in statusLeft).  Per §5.1.1 the
 * truncation walks forward over whole characters.
 *
 * Widths come from charAdvance, the single rule (#117 R1); the total
 * and the drop-walk share one accumulation so the function agrees
 * with itself.  A tab is priced by tab stop here where stringWidth's
 * legacy vocabulary prices it 2; a name containing a tab may
 * therefore be re-truncated by statusLeft's own budget check.  The
 * output is whole characters either way. */
char *leftTruncate(const char *s, int max_width) {
	if (max_width < 1)
		max_width = 1;
	const uint8_t *u = (const uint8_t *)s;
	int len = (int)strlen(s);

	if (utf8WidthN(u, len) <= max_width)
		return xstrdup(s);

	/* With no room for "...", keep the bare rightmost characters
	 * that fit; otherwise the tail budget is what "..." leaves. */
	const char *pre = (max_width > 3) ? "..." : "";
	int budget = (max_width > 3) ? max_width - 3 : max_width;
	int i = utf8DropToFit(u, len, budget);

	size_t pre_len = strlen(pre);
	size_t tail_len = (size_t)(len - i);
	char *r = xmalloc(pre_len + tail_len + 1);
	memcpy(r, pre, pre_len);
	memcpy(r + pre_len, s + i, tail_len);
	r[pre_len + tail_len] = '\0';
	return r;
}

/* How many display columns are available for a filename in a
 * status message?
 */
int nameFit(const char *name, int formatted_len) {
	int chrome = formatted_len - (int)strlen(name);
	int budget = E.screencols - chrome;
	return budget > 8 ? budget : 8;
}

/* Build a middle-truncated display name for a colliding pair.
 *
 * Compare paths from basename upward.  Find the first directory that
 * differs.  Replace shared directories below it with "...".  Keep
 * everything from the differing directory upward (the full prefix).
 * Then left-truncate the whole result to fit.
 *
 *   a/b/c/dira/shared/file.c  vs  a/b/c/dirb/shared/file.c
 *   "shared" same → "..."
 *   "dira" differs → stop. Keep "a/b/c/dira" as full prefix.
 *   Result: a/b/c/dira/.../file.c  (left-truncated if too wide)
 */
static char *middleTruncate(const char *full, const char *other,
			    int max_width) {
	const char *base_a = baseName(full);
	const char *base_b = baseName(other);
	if (base_a == full)
		return leftTruncate(full, max_width);

	int nca = 0, ncb = 0;
	const char *ca_start[64] = { 0 }, *cb_start[64] = { 0 };
	int ca_len[64] = { 0 }, cb_len[64] = { 0 };

	for (const char *p = full; p < base_a && nca < 64;) {
		const char *sl = strchr(p, '/');
		if (!sl || sl >= base_a)
			break;
		ca_start[nca] = p;
		ca_len[nca] = (int)(sl - p);
		nca++;
		p = sl + 1;
	}
	for (const char *p = other; p < base_b && ncb < 64;) {
		const char *sl = strchr(p, '/');
		if (!sl || sl >= base_b)
			break;
		cb_start[ncb] = p;
		cb_len[ncb] = (int)(sl - p);
		ncb++;
		p = sl + 1;
	}

	/* Walk backwards from basename to find the first differing dir. */
	int ia = nca - 1, ib = ncb - 1;
	int diverge_a = 0;
	while (ia >= 0 && ib >= 0) {
		if (ca_len[ia] != cb_len[ib] ||
		    memcmp(ca_start[ia], cb_start[ib], ca_len[ia]) != 0) {
			diverge_a = ia;
			break;
		}
		ia--;
		ib--;
	}
	if (ia >= 0 && ib < 0)
		diverge_a = ia;

	/* The prefix is everything from the start of the path up to
	 * and including the differing directory's trailing slash. */
	const char *prefix_end = ca_start[diverge_a] + ca_len[diverge_a];
	int prefix_len = (int)(prefix_end - full);

	/* Are there shared dirs between the differing dir and basename? */
	int has_shared_below = (diverge_a < nca - 1);

	char mid[1024];
	if (has_shared_below) {
		snprintf(mid, sizeof(mid), "%.*s/.../%.256s", prefix_len, full,
			 base_a);
	} else {
		snprintf(mid, sizeof(mid), "%.*s/%.256s", prefix_len, full,
			 base_a);
	}

	return leftTruncate(mid, max_width);
}

/* Best display form of a filename.  For relative paths that resolve
 * to somewhere under $HOME, use the ~ form if it's shorter.
 * E.g. "../../home/me/foo.c" → "~/foo.c", but "src/main.c" stays.
 * Returns a new string; caller frees. */
static char *displayPath(const char *name) {
	if (name[0] == '/' || name[0] == '~' || name[0] == '*')
		return xstrdup(name);

	char *abs = absolutePath(name);
	char *tilded = collapseHome(abs);
	free(abs);

	if (tilded[0] == '~' && strlen(tilded) < strlen(name))
		return tilded;

	free(tilded);
	return xstrdup(name);
}

/* Compute display_name and min_name_len for every buffer.
 *
 * Called on buffer open/close/rename and on terminal resize.
 *
 * display_name: the name shown in the status bar and switch-buffer.
 * min_name_len: the fewest chars of display_name the status bar must
 *               show to avoid colliding with another buffer's name. */
void computeDisplayNames(void) {
	int max_width = E.screencols - 15;
	if (max_width < 4)
		max_width = 4;

	/* Pass 1: best display form, then left-truncate to fit. */
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
		free(b->display_name);
		const char *name = b->filename ? b->filename : "*scratch*";
		char *dp = displayPath(name);
		b->display_name = leftTruncate(dp, max_width);
		free(dp);
	}

	/* Pass 2: disambiguate collisions via middle-truncate. */
	for (struct buffer *a = E.headbuf; a != NULL; a = a->next) {
		const char *a_raw = a->filename ? a->filename : "*scratch*";
		char *a_full = displayPath(a_raw);
		if (strcmp(a->display_name, a_full) == 0) {
			free(a_full);
			continue;
		}

		for (struct buffer *b = a->next; b != NULL; b = b->next) {
			if (strcmp(a->display_name, b->display_name) != 0)
				continue;

			const char *b_raw = b->filename ? b->filename :
							  "*scratch*";
			char *b_full = displayPath(b_raw);
			free(a->display_name);
			a->display_name =
				middleTruncate(a_full, b_full, max_width);
			free(b->display_name);
			b->display_name =
				middleTruncate(b_full, a_full, max_width);
			free(b_full);
		}
		free(a_full);
	}

	/* Pass 3: compute min_name_len for each buffer.
	 * Find the shortest right-end of display_name that doesn't
	 * match any other buffer's right-end at the same length. */
	for (struct buffer *a = E.headbuf; a != NULL; a = a->next) {
		int alen = strlen(a->display_name);
		const char *bn = baseName(a->display_name);
		a->min_name_len = strlen(bn); /* at least the basename */

		for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
			if (b == a)
				continue;
			int blen = strlen(b->display_name);

			/* Walk from basename length upward until they differ */
			for (int n = a->min_name_len; n <= alen; n++) {
				const char *a_tail = a->display_name + alen - n;
				const char *b_tail = b->display_name + blen - n;
				if (n > blen || strncmp(a_tail, b_tail, n) != 0)
					break;
				/* Still matches at length n, need n+1 */
				if (n + 1 > a->min_name_len)
					a->min_name_len = n + 1;
			}
		}
		if (a->min_name_len > alen)
			a->min_name_len = alen;
	}
}

void clampToBuffer(struct buffer *buf, int *px, int *py) {
	if (*py >= buf->numrows) {
		*py = buf->numrows - 1;
		*px = buf->row[*py].size;
	} else if (*py >= 0 && *px > buf->row[*py].size) {
		*px = buf->row[*py].size;
	}
	/* The mark is a position like any other and can be carried
	 * across rows by an edit, so it snaps to a character boundary
	 * on the same rule as the cursor. */
	if (*py >= 0 && *py < buf->numrows)
		*px = utf8_snapToBoundary(buf->row[*py].chars,
					  buf->row[*py].size, *px, +1);
}

/* Clamp cursor and mark to valid buffer positions. Called after every command.*/
void clampPositions(struct buffer *buf) {
	/* cy < numrows: every position the cursor can hold names a real
	 * row.  numrows >= 1, so numrows - 1 is always valid. */
	if (buf->cy > buf->numrows - 1)
		buf->cy = buf->numrows - 1;
	if (buf->cx > buf->row[buf->cy].size)
		buf->cx = buf->row[buf->cy].size;

	/* A byte offset is a legal cursor position only at the start of
	 * a character or at end of line.  Enforced here, after every
	 * command, rather than trusted to each of the many places that
	 * move a cursor: any command that carries cx from one row to
	 * another can otherwise leave it inside a multibyte character. */
	buf->cx = utf8_snapToBoundary(buf->row[buf->cy].chars,
				      buf->row[buf->cy].size, buf->cx, +1);

	/* Clamp mark */
	if (buf->marky >= 0)
		clampToBuffer(buf, &buf->markx, &buf->marky);
}

/* See the contract in buffer.h.  These define the flat offset space
 * against the row array so callers can migrate to offsets before the
 * storage changes. */

size_t bufTextLen(struct buffer *bufr) {
	size_t total = 0;
	for (int i = 0; i < bufr->numrows; i++)
		total += (size_t)bufr->row[i].size;
	/* One separator between each pair of rows.  Matches
	 * rowsToString(), which emits '\n' before every row but the
	 * first, so an N-row buffer carries N-1 separators. */
	if (bufr->numrows > 1)
		total += (size_t)bufr->numrows - 1;
	return total;
}

size_t bufOffset(struct buffer *bufr, int cx, int cy) {
	if (cy < 0)
		return 0;
	if (cy > bufr->numrows - 1)
		cy = bufr->numrows - 1;
	size_t off = 0;
	for (int i = 0; i < cy; i++)
		off += (size_t)bufr->row[i].size + 1;
	if (cx < 0)
		cx = 0;
	if (cx > bufr->row[cy].size)
		cx = bufr->row[cy].size;
	return off + (size_t)cx;
}

void bufPos(struct buffer *bufr, size_t off, int *cx, int *cy) {
	size_t walked = 0;
	for (int i = 0; i < bufr->numrows; i++) {
		size_t rowlen = (size_t)bufr->row[i].size;
		/* off == walked + rowlen is the end-of-row position,
		 * which belongs to this row rather than to the start of
		 * the next: the cursor sits after the last byte, not
		 * before the first byte of what follows.  The last row
		 * is the only one where that position is also the end
		 * of the buffer. */
		if (off <= walked + rowlen) {
			*cy = i;
			*cx = (int)(off - walked);
			return;
		}
		walked += rowlen + 1; /* + the separator newline */
	}
	/* Past the end: clamp to the last valid position.  numrows >= 1
	 * is an invariant, so row[numrows - 1] exists. */
	*cy = bufr->numrows - 1;
	*cx = bufr->row[*cy].size;
}
