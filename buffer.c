#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "emil.h"
#include "message.h"
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

extern struct config E;

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
	char *iopath = expandTilde(buf->filename);
	(void)lockFile(buf, iopath);
	free(iopath);
}

void markBufferClean(struct buffer *buf) {
	if (!buf->dirty)
		return;
	buf->dirty = 0;
	if (buf->lock_fd >= 0)
		releaseLock(buf);
}

void insertRow(struct buffer *bufr, int at, char *s, size_t len) {
	if (at < 0 || at > bufr->numrows)
		return;

	if (bufr->numrows >= bufr->rowcap) {
		int new_cap = bufr->rowcap ? bufr->rowcap * 2 : 16;
		bufr->row = xrealloc(bufr->row, sizeof(erow) * new_cap);
		memset(&bufr->row[bufr->rowcap], 0,
		       sizeof(erow) * (new_cap - bufr->rowcap));
		bufr->rowcap = new_cap;
	}

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
	invalidateScreenCache(bufr);
}

void freeRow(erow *row) {
	free(row->chars);
}

void delRow(struct buffer *bufr, int at) {
	if (bufr->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

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
	invalidateScreenCache(bufr);
}

void rowInsertChar(struct buffer *bufr, erow *row, int at, int c) {
	if (at < 0 || at > row->size)
		at = row->size;

	int needed = row->size + 2;
	if (needed > row->charcap) {
		int new_cap = row->charcap < 16 ? 16 : row->charcap * 2;
		if (new_cap < needed)
			new_cap = needed;
		row->chars = xrealloc(row->chars, new_cap);
		row->charcap = new_cap;
	}
	memmove(&row->chars[at + 1], &row->chars[at], row->size - at + 1);
	row->size++;
	row->chars[at] = c;
	markBufferDirty(bufr);
	row->cached_width = -1;
	invalidateScreenCache(bufr);
}

void rowInsertUnicode(struct buffer *bufr, erow *row, int at) {
	if (bufr->read_only) {
		setStatusMessage(msg_read_only);
		return;
	}

	if (at < 0 || at > row->size)
		at = row->size;
	int needed = row->size + 1 + E.nunicode;
	if (needed > row->charcap) {
		int new_cap = row->charcap < 16 ? 16 : row->charcap * 2;
		if (new_cap < needed)
			new_cap = needed;
		row->chars = xrealloc(row->chars, new_cap);
		row->charcap = new_cap;
	}
	memmove(&row->chars[at + E.nunicode], &row->chars[at],
		row->size - at + 1);
	row->size += E.nunicode;
	memcpy(&row->chars[at], E.unicode, E.nunicode);
	row->cached_width = -1;
	markBufferDirty(bufr);
	invalidateScreenCache(bufr);
}

void rowAppendString(struct buffer *bufr, erow *row, char *s, size_t len) {
	/* Guard against int overflow: row->size is int */
	if (len > (size_t)(INT_MAX - row->size - 1))
		return;
	int needed = row->size + (int)len + 1;
	if (needed > row->charcap) {
		int new_cap = row->charcap < 16 ? 16 : row->charcap * 2;
		if (new_cap < needed)
			new_cap = needed;
		row->chars = xrealloc(row->chars, new_cap);
		row->charcap = new_cap;
	}
	memcpy(&row->chars[row->size], s, len);
	row->size += len;
	row->chars[row->size] = '\0';
	row->cached_width = -1;
	markBufferDirty(bufr);
	invalidateScreenCache(bufr);
}

void rowDelChar(struct buffer *bufr, erow *row, int at) {
	if (at < 0 || at >= row->size)
		return;
	/* at + size <= row->size is guaranteed: all input paths enforce
	 * valid UTF-8, and all callers pass at on a character boundary. */
	int size = utf8_nBytes(row->chars[at]);
	memmove(&row->chars[at], &row->chars[at + size],
		row->size - ((at + size) - 1));
	row->size -= size;
	row->cached_width = -1;
	markBufferDirty(bufr);
	invalidateScreenCache(bufr);
}

struct buffer *newBuffer(void) {
	struct buffer *ret = xmalloc(sizeof(struct buffer));
	ret->indent = 0;
	ret->markx = -1;
	ret->marky = -1;
	ret->mark_active = 0;
	ret->mark_ring_len = 0;
	ret->mark_ring_idx = 0;
	ret->cx = 0;
	ret->cy = 0;
	ret->numrows = 0;
	ret->rowcap = 0;
	ret->row = NULL;
	ret->filename = NULL;
	ret->display_name = NULL;
	ret->min_name_len = 0;
	ret->query = NULL;
	ret->dirty = 0;
	ret->special_buffer = 0;
	ret->undo = newUndo();
	ret->redo = NULL;
	ret->undo_count = 1;
	ret->completion_state.last_completed_text = NULL;
	ret->completion_state.completion_start_pos = 0;
	ret->completion_state.successive_tabs = 0;
	ret->completion_state.last_completion_count = 0;
	ret->completion_state.preserve_message = 0;
	ret->completion_state.selected = -1;
	ret->completion_state.matches = NULL;
	ret->completion_state.n_matches = 0;
	ret->next = NULL;
	ret->word_wrap = 0;
	ret->rectangle_mode = 0;
	ret->single_line = 0;
	ret->screen_line_start = NULL;
	ret->screen_line_cache_size = 0;
	ret->screen_line_cache_valid = 0;
	ret->read_only = 0;
	ret->lock_fd = -1;
	ret->open_mtime = 0;
	ret->file_size = 0;
	ret->external_mod = 0;
	ret->internal_mod = 0;
	ret->undo_pruned = 0;
	return ret;
}

void destroyBuffer(struct buffer *buf) {
	if (E.lastVisitedBuffer == buf)
		E.lastVisitedBuffer = NULL;
	releaseLock(buf);
	clearUndosAndRedos(buf);
	free(buf->filename);
	free(buf->display_name);
	free(buf->query);
	free(buf->screen_line_start);
	free(buf->completion_state.last_completed_text);
	if (buf->completion_state.matches) {
		for (int i = 0; i < buf->completion_state.n_matches; i++)
			free(buf->completion_state.matches[i]);
		free(buf->completion_state.matches);
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
	invalidateScreenCache(buf);
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
	buf->next = E.headbuf;
	E.headbuf = buf;
	return buf;
}

void clearBuffer(struct buffer *buf) {
	int was_read_only = buf->read_only;
	buf->read_only = 0;
	while (buf->numrows > 0)
		delRow(buf, 0);
	buf->read_only = was_read_only;
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
		snprintf(prompt, sizeof(prompt),
			 "Switch to buffer (default %s): %%s", base);
	} else {
		snprintf(prompt, sizeof(prompt), "Switch to buffer: %%s");
	}

	uint8_t *buffer_name =
		editorPrompt(E.buf, (uint8_t *)prompt, PROMPT_BUFFER, NULL);

	if (buffer_name == NULL) {
		setStatusMessage(msg_buffer_switch_canceled);
		return;
	}

	struct buffer *targetBuffer = NULL;

	if (buffer_name[0] == '\0') {
		/* User pressed Enter without typing — use default */
		targetBuffer = defaultBuffer;
		if (!targetBuffer) {
			setStatusMessage(msg_no_buffer_switch);
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
			setStatusMessage(msg_no_buffer_named, buffer_name);
			free(buffer_name);
			return;
		}
	}

	E.lastVisitedBuffer = E.buf;
	E.buf = targetBuffer;

	const char *full = E.buf->filename ? E.buf->filename : "*scratch*";
	int n = snprintf(NULL, 0, msg_switched_to, full);
	char *switchedName = leftTruncate(full, nameFit(full, n));
	setStatusMessage(msg_switched_to, switchedName);
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
}

void killBuffer(void) {
	struct buffer *bufr = E.buf;

	// Bypass confirmation for special buffers
	if (bufr->dirty && bufr->filename != NULL && !bufr->special_buffer) {
		const char *kill_fmt =
			"Buffer %s modified; kill anyway? (y or n)";
		const char *fname = bufr->filename ? bufr->filename :
						     "*scratch*";
		int n = snprintf(NULL, 0, kill_fmt, fname);
		char *killName = leftTruncate(fname, nameFit(fname, n));
		setStatusMessage(kill_fmt, killName);
		free(killName);
		refreshScreen();
		int c = readKey();
		if (c != 'y' && c != 'Y') {
			setStatusMessage("");
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

	// Update window focus
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->buf == bufr) {
			// If it's the last buffer, create a new scratch buffer
			if (bufr->next == NULL && prevBuf == NULL) {
				E.windows[i]->buf = newBuffer();
				E.windows[i]->buf->filename =
					xstrdup("*scratch*");
				E.windows[i]->buf->special_buffer = 1;
				E.headbuf = E.windows[i]->buf;
				E.buf = E.headbuf; // Ensure E.buf is updated
			} else if (bufr->next == NULL) {
				E.windows[i]->buf = E.headbuf;
				E.buf = E.headbuf; // Ensure E.buf is updated
			} else {
				E.windows[i]->buf = bufr->next;
				E.buf = bufr->next; // Ensure E.buf is updated
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
	computeDisplayNames();
}

/* Basename helper: returns pointer into path after the last '/'. */
static const char *baseName(const char *path) {
	const char *slash = strrchr(path, '/');
	return slash ? slash + 1 : path;
}

/* Left-truncate a string to fit in max_width, prepending "...".
 * Always returns a string that fits in max_width.
 * Returns a newly allocated string. */
char *leftTruncate(const char *s, int max_width) {
	if (max_width < 1)
		max_width = 1;
	int len = (int)strlen(s);
	if (len <= max_width)
		return xstrdup(s);
	if (max_width <= 3) {
		/* No room for "..." prefix, just return the tail */
		char *r = xstrdup(s + (len - max_width));
		return r;
	}
	int tail = max_width - 3;
	char *r = xmalloc(max_width + 1);
	snprintf(r, max_width + 1, "...%s", s + (len - tail));
	return r;
}

/* How many display columns are available for a filename in a
 * status message?
 *
 * |name|           — the full filename
 * |formatted_len|  — the result of snprintf(NULL,0,fmt,...) with
 *                    the full name already included
 *
 * The caller measures the fully-formatted message, we subtract
 * the name to get the chrome width, then return screencols minus
 * that.  Exact, locale-proof, no magic numbers.
 *
 * Typical usage:
 *   int n = snprintf(NULL, 0, msg_wrote_bytes, len, filename);
 *   char *show = leftTruncate(filename, nameFit(filename, n));
 *   setStatusMessage(msg_wrote_bytes, len, show);
 *   free(show);
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
		/* Differing dir is directly above basename — no gap. */
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
	if (buf->numrows == 0) {
		*py = 0;
		*px = 0;
	} else if (*py >= buf->numrows) {
		*py = buf->numrows - 1;
		*px = buf->row[*py].size;
	} else if (*py >= 0 && *px > buf->row[*py].size) {
		*px = buf->row[*py].size;
	}
}

/* Clamp cursor and mark to valid buffer positions.
 * Called after every command to prevent out-of-bounds
 * row access in rendering or subsequent commands. */
void clampPositions(struct buffer *buf) {
	if (buf->numrows == 0) {
		buf->cy = 0;
		buf->cx = 0;
		buf->markx = -1;
		buf->marky = -1;
		buf->mark_active = 0;
		return;
	}
	/* cy == numrows is allowed (virtual EOF line) but nothing beyond */
	if (buf->cy > buf->numrows)
		buf->cy = buf->numrows;
	if (buf->cy < buf->numrows && buf->cx > buf->row[buf->cy].size)
		buf->cx = buf->row[buf->cy].size;
	if (buf->cy == buf->numrows)
		buf->cx = 0;

	/* Clamp mark */
	if (buf->marky >= 0)
		clampToBuffer(buf, &buf->markx, &buf->marky);
}
