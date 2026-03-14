#include "emil.h"
#include "message.h"
#include "fileio.h"
#include "buffer.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/select.h>
#include "display.h"
#include "prompt.h"
#include "util.h"
#include "undo.h"
#include "unicode.h"
#include "keymap.h"
#include "unused.h"
#include <limits.h>

/* Access global editor state */
extern struct editorConfig E;

/* External functions we need */
extern void die(const char *s);

/*** file locking ***/

/* Try to acquire an advisory write lock on a file.
 * Returns 0 on success (lock acquired), -1 if already locked (sets
 * status message with the blocking PID), or -2 on error.
 * On success, bufr->lock_fd is set and must be released later. */

int editorLockFile(struct editorBuffer *bufr, const char *filename) {
	/* Try O_RDWR first (needed for F_WRLCK per POSIX).
	 * Fall back to O_RDONLY + F_RDLCK if the file isn't writable. */
	int fd = open(filename, O_RDWR);
	int use_rdlck = 0;
	if (fd < 0) {
		if (errno == ENOENT)
			return -2; /* file doesn't exist yet — nothing to lock */
		fd = open(filename, O_RDONLY);
		if (fd < 0)
			return -2;
		use_rdlck = 1;
	}

	struct flock fl;
	memset(&fl, 0, sizeof(fl));
	fl.l_type = use_rdlck ? F_RDLCK : F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0; /* whole file */

	if (fcntl(fd, F_SETLK, &fl) == 0) {
		/* Lock acquired */
		bufr->lock_fd = fd;

		/* Record mtime for external modification detection */
		struct stat st;
		if (fstat(fd, &st) == 0)
			bufr->open_mtime = st.st_mtime;

		return 0;
	}

	/* Lock failed — find out who holds it */
	if (errno == EACCES || errno == EAGAIN) {
		struct flock query;
		memset(&query, 0, sizeof(query));
		query.l_type = F_WRLCK;
		query.l_whence = SEEK_SET;
		query.l_start = 0;
		query.l_len = 0;

		if (fcntl(fd, F_GETLK, &query) == 0 &&
		    query.l_type != F_UNLCK) {
			editorSetStatusMessage(msg_file_locked,
					       (int)query.l_pid);
		} else {
			editorSetStatusMessage(msg_file_locked, 0);
		}
	}

	close(fd);
	return -1;
}

/* Release the advisory lock held by this buffer. */

void editorReleaseLock(struct editorBuffer *bufr) {
	if (bufr->lock_fd >= 0) {
		close(bufr->lock_fd);
		bufr->lock_fd = -1;
	}
	bufr->open_mtime = 0;
	bufr->external_mod = 0;
}

/* Check whether the underlying file has been modified externally.
 * Called periodically (e.g. from refreshScreen).  Sets bufr->external_mod
 * and fires a one-time status message. */

void editorCheckFileModified(struct editorBuffer *bufr) {
	if (bufr->filename == NULL || bufr->open_mtime == 0)
		return;
	if (bufr->external_mod)
		return; /* already flagged */

	struct stat st;
	if (stat(bufr->filename, &st) == 0) {
		if (st.st_mtime != bufr->open_mtime) {
			bufr->external_mod = 1;
			editorSetStatusMessage(msg_file_changed_on_disk);
		}
	}
}

/*** file i/o ***/

char *editorRowsToString(struct editorBuffer *bufr, int *buflen) {
	int totlen = 0;
	int j;
	for (j = 0; j < bufr->numrows; j++) {
		totlen += bufr->row[j].size + 1;
	}
	*buflen = totlen;

	char *buf = xmalloc(totlen);
	char *p = buf;
	for (j = 0; j < bufr->numrows; j++) {
		memcpy(p, bufr->row[j].chars, bufr->row[j].size);
		p += bufr->row[j].size;
		*p = '\n';
		p++;
	}

	return buf;
}

/* Validate UTF-8 in the buffer and check for null bytes.
 * Also rejects overlong encodings, surrogates (U+D800-U+DFFF),
 * and codepoints above U+10FFFF.
 * Returns 1 if valid, 0 if invalid. */

static int checkUTF8Validity(struct editorBuffer *bufr) {
	for (int row = 0; row < bufr->numrows; row++) {
		if (!utf8_validate(bufr->row[row].chars, bufr->row[row].size))
			return 0;
	}
	return 1;
}

/* Pre-scan an open file for null bytes.  Returns 1 if null bytes
 * are found, 0 if clean.  Rewinds the file on return.
 * This is needed because emil_getline uses fgets/strlen internally,
 * which treats '\0' as a string terminator and would silently
 * truncate lines containing null bytes. */

static int fileContainsNullBytes(FILE *fp) {
	unsigned char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		if (memchr(buf, '\0', n) != NULL) {
			rewind(fp);
			return 1;
		}
	}
	rewind(fp);
	return 0;
}

/* Open a file into a buffer.
 * Returns 0 on success, -1 on failure (file not found is not a failure;
 * the buffer is left empty with the filename set). */

int editorOpen(struct editorBuffer *bufr, char *filename) {
	free(bufr->filename);
	bufr->filename = xstrdup(filename);

	FILE *fp = fopen(filename, "r");
	if (!fp) {
		if (errno == ENOENT) {
			editorSetStatusMessage(msg_new_file, bufr->filename);
			return 0;
		}
		editorSetStatusMessage(msg_cant_open, strerror(errno));
		free(bufr->filename);
		bufr->filename = NULL;
		return -1;
	}

	/* Pre-scan for null bytes before line-based reading because
	 * emil_getline (fgets/strlen) silently truncates at '\0'. */
	if (fileContainsNullBytes(fp)) {
		fclose(fp);
		editorSetStatusMessage(
			"File failed UTF-8 validation (contains null bytes)");
		free(bufr->filename);
		bufr->filename = NULL;
		return -1;
	}

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while ((linelen = emil_getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 &&
		       (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen--;
		editorInsertRow(bufr, bufr->numrows, line, linelen);
	}

	/* Get the display length of the longest column */
	int max_width = 0;
	for (int i = 0; i < bufr->numrows; i++) {
		int w = calculateLineWidth(&bufr->row[i]);
		if (w > max_width)
			max_width = w;
	}

	free(line);
	fclose(fp);

	/* Validate UTF-8 encoding of the loaded content */
	if (!checkUTF8Validity(bufr)) {
		/* Clean up: free rows from the end to avoid O(n^2) shifting */
		for (int i = bufr->numrows - 1; i >= 0; i--) {
			freeRow(&bufr->row[i]);
		}
		bufr->numrows = 0;
		free(bufr->filename);
		bufr->filename = NULL;
		editorSetStatusMessage(msg_file_bad_utf8);
		return -1;
	}

	bufr->dirty = 0;
	/* If the file is not writable by us, mark buffer read-only */
	if (access(filename, W_OK) != 0) {
		bufr->read_only = 1;
	}

	/* Try to acquire advisory lock */
	int lock_result = editorLockFile(bufr, filename);
	if (lock_result == -1) {
		/* File is locked by another process — open read-only */
		bufr->read_only = 1;
	} else if (lock_result == 0) {
		/* Lock acquired — mtime already recorded by editorLockFile */
	}

	computeDisplayNames();

	/* Enable word wrap by default for prose-oriented file types */
	if (bufr->filename) {
		char *ext = strrchr(bufr->filename, '.');
		if (ext) {
			if (strcmp(ext, ".org") == 0 ||
			    strcmp(ext, ".md") == 0 ||
			    strcmp(ext, ".txt") == 0 ||
			    strcmp(ext, ".fountain") == 0) {
				bufr->word_wrap = 1;
			}
		}
	}

	editorSetStatusMessage("%d lines, %d columns", bufr->numrows,
			       max_width);
	return 0;
}

void editorRevert(struct editorConfig *ed, struct editorBuffer *buf) {
	struct editorBuffer *new = newBuffer();
	if (editorOpen(new, buf->filename) < 0) {
		/* Open/validation failed — keep the current buffer */
		destroyBuffer(new);
		return;
	}
	new->next = buf->next;
	ed->buf = new;
	if (ed->headbuf == buf) {
		ed->headbuf = new;
	}
	struct editorBuffer *cur = ed->headbuf;
	while (cur != NULL) {
		if (cur->next == buf) {
			cur->next = new;
			break;
		}
		cur = cur->next;
	}
	for (int i = 0; i < ed->nwindows; i++) {
		if (ed->windows[i]->buf == buf) {
			ed->windows[i]->buf = new;
		}
	}
	new->indent = buf->indent;
	new->cx = buf->cx;
	new->cy = buf->cy;
	if (new->numrows == 0) {
		new->cy = 0;
		new->cx = 0;
	} else if (new->cy >= new->numrows) {
		new->cy = new->numrows - 1;
		new->cx = 0;
	} else if (new->cx > new->row[new->cy].size) {
		new->cx = new->row[new->cy].size;
	}
	destroyBuffer(buf);
}

void editorSave(struct editorBuffer *bufr) {
	if (bufr->filename == NULL) {
		bufr->filename = (char *)editorPrompt(
			bufr, (uint8_t *)"Save as: %s", PROMPT_FILES, NULL);
		if (bufr->filename == NULL) {
			editorSetStatusMessage("Save aborted.");
			return;
		}
		bufr->special_buffer = 0;
		computeDisplayNames();
	}

	int len;
	char *buf = editorRowsToString(bufr, &len);

	/* Build temp filename: <filename>.tmpXXXXXX */
	char tmpname[PATH_MAX];
	snprintf(tmpname, sizeof(tmpname), "%s.tmpXXXXXX", bufr->filename);

	int fd = mkstemp(tmpname);
	if (fd == -1) {
		free(buf);
		editorSetStatusMessage("Save failed: %s", strerror(errno));
		return;
	}

	/* Preserve permissions if file already exists */
	struct stat st;
	if (stat(bufr->filename, &st) == 0) {
		fchmod(fd, st.st_mode);
	}

	/* Write buffer fully, handling partial writes and EINTR */
	ssize_t total = 0;
	while (total < len) {
		ssize_t n = write(fd, buf + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue; // interrupted, try again
			close(fd);
			unlink(tmpname);
			free(buf);
			editorSetStatusMessage("Save failed: %s",
					       strerror(errno));
			return;
		} else if (n == 0) {
			// shouldn't happen for regular files, treat as error
			close(fd);
			unlink(tmpname);
			free(buf);
			editorSetStatusMessage(
				"Save failed: wrote 0 bytes unexpectedly");
			return;
		}
		total += n;
	}

	if (fsync(fd) == -1) {
		close(fd);
		unlink(tmpname);
		free(buf);
		editorSetStatusMessage("Save failed: %s", strerror(errno));
		return;
	}

	close(fd);

	if (rename(tmpname, bufr->filename) == -1) {
		unlink(tmpname);
		free(buf);
		editorSetStatusMessage("Save failed: %s", strerror(errno));
		return;
	}

	free(buf);
	bufr->dirty = 0;

	/* Update stored mtime after save */
	struct stat save_st;
	if (stat(bufr->filename, &save_st) == 0)
		bufr->open_mtime = save_st.st_mtime;
	bufr->external_mod = 0;
	bufr->internal_mod = 1;

	/* If we didn't have a lock yet (new file), acquire one now */
	if (bufr->lock_fd < 0)
		editorLockFile(bufr, bufr->filename);

	/* TODO Interactive fallback to direct write if temp creation fails */
	/* TODO: fsync parent dir after rename */

	editorSetStatusMessage(msg_wrote_bytes, len, bufr->filename);
}

void editorSaveAs(struct editorBuffer *bufr) {
	char *new_filename = (char *)editorPrompt(
		bufr, (uint8_t *)"Save as: %s", PROMPT_FILES, NULL);
	if (new_filename == NULL) {
		editorSetStatusMessage("Save aborted.");
		return;
	}
	free(bufr->filename);
	bufr->filename = new_filename;
	computeDisplayNames();
	editorSave(bufr);
}

/* Switch the focused window to the named file.  If a buffer with that
 * filename already exists, reuse it; otherwise open a new one.
 * Returns the buffer on success, NULL on failure. */
struct editorBuffer *editorSwitchToFile(const char *filename) {
	/* Check if already open */
	for (struct editorBuffer *buf = E.headbuf; buf; buf = buf->next) {
		if (buf->filename && strcmp(buf->filename, filename) == 0) {
			E.buf = buf;
			E.windows[windowFocusedIdx()]->buf = buf;
			return buf;
		}
	}

	/* Open new buffer */
	struct editorBuffer *nb = newBuffer();
	if (editorOpen(nb, (char *)filename) < 0) {
		destroyBuffer(nb);
		return NULL;
	}
	nb->next = E.headbuf;
	E.headbuf = nb;
	E.buf = nb;
	E.windows[windowFocusedIdx()]->buf = nb;
	return nb;
}

void findFile(void) {
	uint8_t *prompt =
		editorPrompt(E.buf, "Find File: %s", PROMPT_FILES, NULL);

	if (prompt == NULL) {
		editorSetStatusMessage(msg_canceled);
		return;
	}
	// I think this probably never gets called anymore
	if (prompt[strlen(prompt) - 1] == '/') {
		editorSetStatusMessage("Directory editing not supported.");
		free(prompt);
		return;
	}

	struct editorBuffer *buf = editorSwitchToFile((char *)prompt);
	computeDisplayNames();
	free(prompt);
	if (buf)
		refreshScreen();
}

void editorInsertFile(struct editorConfig *UNUSED(ed),
		      struct editorBuffer *buf) {
	uint8_t *filename =
		editorPrompt(buf, "Insert file: %s", PROMPT_FILES, NULL);
	if (filename == NULL) {
		return;
	}

	FILE *fp = fopen((char *)filename, "r");
	if (!fp) {
		if (errno == ENOENT) {
			editorSetStatusMessage("File not found: %s", filename);
		} else {
			editorSetStatusMessage("Error opening file: %s",
					       strerror(errno));
		}
		free(filename);
		return;
	}

	/* Pre-scan for null bytes */
	if (fileContainsNullBytes(fp)) {
		fclose(fp);
		editorSetStatusMessage(
			"File failed UTF-8 validation (contains null bytes)");
		free(filename);
		return;
	}

	/* Load into a temporary buffer so we can validate before
	 * modifying the real buffer */
	struct editorBuffer *tmpbuf = newBuffer();

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while ((linelen = emil_getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
				       line[linelen - 1] == '\r')) {
			linelen--;
		}
		editorInsertRow(tmpbuf, tmpbuf->numrows, line, linelen);
	}

	free(line);
	fclose(fp);

	/* Validate UTF-8 before inserting */
	if (!checkUTF8Validity(tmpbuf)) {
		destroyBuffer(tmpbuf);
		editorSetStatusMessage(msg_file_bad_utf8);
		free(filename);
		return;
	}

	/* Now insert the validated content into the actual buffer */
	int saved_cy = buf->cy;
	int lines_inserted = 0;

	for (int i = 0; i < tmpbuf->numrows; i++) {
		editorInsertRow(buf, saved_cy + lines_inserted,
				(char *)tmpbuf->row[i].chars,
				tmpbuf->row[i].size);
		lines_inserted++;
	}

	destroyBuffer(tmpbuf);

	if (lines_inserted > 0) {
		buf->cy = saved_cy + lines_inserted - 1;
		buf->cx = buf->row[buf->cy].size;
	}

	editorSetStatusMessage("Inserted %d lines from %s", lines_inserted,
			       filename);
	free(filename);

	buf->dirty++;
}

/* Compute the relative path from directory 'from' to directory 'to'.
 * Both must be absolute paths.  Returns a malloc'd string.
 * Example: from="/a/b/c", to="/a/d" => "../../d" */
char *relativePath(const char *from, const char *to) {
	/* Find the common prefix, breaking on '/' boundaries.
	 * split will point just past the last shared '/' separator,
	 * so from[split..] and to[split..] are the diverging tails. */
	int split = 0;
	int i = 0;
	while (from[i] && to[i] && from[i] == to[i]) {
		if (from[i] == '/')
			split = i + 1;
		i++;
	}
	/* Handle one being a prefix of the other:
	 * e.g. from="/a/b/c" to="/a/b"  (to ends, from continues with '/')
	 *      from="/a/b"   to="/a/b/c" (from ends, to continues with '/')
	 *      from="/a/b"   to="/a/b"   (both end) */
	if (from[i] == '\0' && to[i] == '\0')
		split = i;
	else if (from[i] == '\0' && to[i] == '/')
		split = i;
	else if (to[i] == '\0' && from[i] == '/')
		split = i + 1;

	/* Count directory segments remaining in 'from' after split */
	int ups = 0;
	for (int j = split; from[j]; j++) {
		if (from[j] == '/' && from[j + 1] != '\0')
			ups++;
	}
	if (from[split] != '\0')
		ups++;

	/* Tail of 'to' after split — careful not to read past end */
	const char *to_tail = "";
	if ((int)strlen(to) > split) {
		to_tail = to + split;
		if (*to_tail == '/')
			to_tail++;
	}

	int tail_len = strlen(to_tail);
	int result_len = ups * 3 + tail_len + 1;
	char *result = xmalloc(result_len);
	result[0] = '\0';

	for (int j = 0; j < ups; j++)
		emil_strlcat(result, "../", result_len);

	if (tail_len > 0)
		emil_strlcat(result, to_tail, result_len);
	else if (ups > 0)
		result[strlen(result) - 1] = '\0'; /* trim trailing / */

	return result;
}

/* Canonicalize an absolute path by resolving . and .. segments.
 * Does NOT resolve symlinks — purely string-level.
 * Modifies the string in place and returns it. */
static char *cleanPath(char *path) {
	/* Stack of pointers to segment starts within path */
	char *segs[256];
	int depth = 0;

	char *p = path;
	if (*p == '/')
		p++;

	while (*p) {
		char *seg = p;
		while (*p && *p != '/')
			p++;
		int len = p - seg;
		if (*p == '/')
			p++;

		if (len == 1 && seg[0] == '.') {
			continue;
		} else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
			if (depth > 0)
				depth--;
		} else {
			if (depth < 256)
				segs[depth++] = seg;
			/* null-terminate this segment for later copy */
			if (seg[len] != '\0')
				seg[len] = '\0';
		}
	}

	/* Reassemble */
	char *out = path;
	*out++ = '/';
	for (int i = 0; i < depth; i++) {
		int slen = strlen(segs[i]);
		memmove(out, segs[i], slen);
		out += slen;
		if (i < depth - 1)
			*out++ = '/';
	}
	*out = '\0';
	return path;
}

/* Rebase a relative filename from old_cwd to new_cwd.
 * Returns a new malloc'd string.  Absolute paths are returned as-is (duped).
 * Used by editorChangeDirectory and exposed for testing. */
char *rebaseFilename(const char *filename, const char *old_cwd,
		     const char *new_cwd) {
	if (filename[0] == '/')
		return xstrdup(filename);

	/* Absolutize against old cwd and clean up any .. segments */
	int abs_len = strlen(old_cwd) + 1 + strlen(filename) + 1;
	char *abs = xmalloc(abs_len);
	snprintf(abs, abs_len, "%s/%s", old_cwd, filename);
	cleanPath(abs);

	/* Relativize the directory part against new cwd,
	 * then reattach the basename */
	char *slash = strrchr(abs, '/');
	char *base = xstrdup(slash + 1);
	*slash = '\0'; /* abs is now the directory */
	char *reldir = relativePath(new_cwd, abs);
	int new_len = strlen(reldir) + 1 + strlen(base) + 1;
	char *new_name = xmalloc(new_len);
	if (reldir[0] == '\0')
		snprintf(new_name, new_len, "%s", base);
	else
		snprintf(new_name, new_len, "%s/%s", reldir, base);

	free(abs);
	free(base);
	free(reldir);
	return new_name;
}

void editorChangeDirectory(struct editorConfig *ed, struct editorBuffer *buf) {
	(void)buf; /* unused parameter */

	uint8_t *dir = editorPrompt(ed->buf, (uint8_t *)"Directory: %s",
				    PROMPT_DIR, NULL);
	if (dir == NULL) {
		editorSetStatusMessage(msg_canceled);
		return;
	}

	/* Grab the old cwd before changing */
	char old_cwd[PATH_MAX];
	if (getcwd(old_cwd, sizeof(old_cwd)) == NULL) {
		editorSetStatusMessage(
			"cd: cannot determine current directory");
		free(dir);
		return;
	}

	if (chdir((char *)dir) != 0) {
		editorSetStatusMessage("cd: %s: %s", (char *)dir,
				       strerror(errno));
		free(dir);
		return;
	}

	char new_cwd[PATH_MAX];
	if (getcwd(new_cwd, sizeof(new_cwd)) == NULL) {
		/* chdir succeeded but getcwd failed — unlikely but
		 * leave filenames as-is */
		editorSetStatusMessage("Changed directory");
		free(dir);
		return;
	}

	/* If the directory actually changed, update relative filenames */
	if (strcmp(old_cwd, new_cwd) != 0) {
		for (struct editorBuffer *b = ed->headbuf; b != NULL;
		     b = b->next) {
			if (b->filename == NULL || b->special_buffer)
				continue;
			char *new_name =
				rebaseFilename(b->filename, old_cwd, new_cwd);
			free(b->filename);
			b->filename = new_name;
		}

		computeDisplayNames();
	}

	editorSetStatusMessage("Current directory: %s", new_cwd);
	free(dir);
}
