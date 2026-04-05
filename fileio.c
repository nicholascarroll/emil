#include "fileio.h"
#include "buffer.h"
#include "display.h"
#include "emil.h"
#include "keymap.h"
#include "message.h"
#include "prompt.h"
#include "terminal.h"
#include "undo.h"
#include "unicode.h"
#include "unused.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* Access global editor state */
extern struct config E;

/* External functions we need */
extern void die(const char *s);

/*** file locking ***/

/* Try to acquire an advisory write lock on a file.
 * Returns 0 on success (lock acquired), -1 if already locked (sets
 * status message with the blocking PID), or -2 on error.
 * On success, bufr->lock_fd is set and must be released later. */

int lockFile(struct buffer *bufr, const char *filename) {
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
			setStatusMessage(msg_file_locked, (int)query.l_pid);
		} else {
			setStatusMessage(msg_file_locked, 0);
		}
	}

	close(fd);
	return -1;
}

/* Release the advisory lock held by this buffer. */

void releaseLock(struct buffer *bufr) {
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

void checkFileModified(void) {
	if (E.buf->filename == NULL || E.buf->open_mtime == 0)
		return;
	if (E.buf->external_mod)
		return; /* already flagged */

	char *iopath = expandTilde(E.buf->filename);
	struct stat st;
	if (stat(iopath, &st) == 0) {
		if (st.st_mtime != E.buf->open_mtime) {
			E.buf->external_mod = 1;
			setStatusMessage(msg_file_changed_on_disk);
		}
	}
	free(iopath);
}

/*** file i/o ***/

char *rowsToString(struct buffer *bufr, size_t *buflen) {
	size_t totlen = 0;
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

static int checkUTF8Validity(struct buffer *bufr) {
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

int editorOpen(struct buffer *bufr, char *filename) {
	free(bufr->filename);
	bufr->filename = collapseHome(filename);

	/* Resolve to an OS-usable path for all I/O in this function */
	char *iopath = expandTilde(bufr->filename);

	FILE *fp = fopen(iopath, "r");
	if (!fp) {
		if (errno == ENOENT) {
			setStatusMessage(msg_new_file, bufr->filename);
			free(iopath);
			return 0;
		}
		setStatusMessage(msg_cant_open, strerror(errno));
		free(bufr->filename);
		bufr->filename = NULL;
		free(iopath);
		return -1;
	}

	/* Check file size against budget (hard limit) */
	{
		struct stat st;
		if (fstat(fileno(fp), &st) == 0 && S_ISREG(st.st_mode)) {
			bufr->file_size = (size_t)st.st_size;
			if (totalOpenBytes() + totalKillBytes() +
				    bufr->file_size >
			    (size_t)EMIL_MAX_OPEN_BYTES) {
				fclose(fp);
				setStatusMessage(msg_memory_limit);
				free(bufr->filename);
				bufr->filename = NULL;
				bufr->file_size = 0;
				free(iopath);
				return -1;
			}
		}
	}

	/* Pre-scan for null bytes before line-based reading because
	 * emil_getline (fgets/strlen) silently truncates at '\0'. */
	if (fileContainsNullBytes(fp)) {
		fclose(fp);
		setStatusMessage(msg_binary_file);
		free(bufr->filename);
		bufr->filename = NULL;
		free(iopath);
		return -1;
	}

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while ((linelen = emil_getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 &&
		       (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen--;
		insertRow(bufr, bufr->numrows, line, linelen);
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

	/* Guard against pathological files with billions of tiny lines. */
	if (bufr->numrows > INT_MAX / 2) {
		for (int i = 0; i < bufr->numrows; i++)
			freeRow(&bufr->row[i]);
		free(bufr->row);
		bufr->row = NULL;
		bufr->numrows = 0;
		bufr->rowcap = 0;
		free(bufr->filename);
		bufr->filename = NULL;
		setStatusMessage("File has too many lines");
		free(iopath);
		return -1;
	}

	/* Validate UTF-8 encoding of the loaded content */
	if (!checkUTF8Validity(bufr)) {
		for (int i = 0; i < bufr->numrows; i++) {
			freeRow(&bufr->row[i]);
		}
		free(bufr->row);
		bufr->row = NULL;
		bufr->numrows = 0;
		bufr->rowcap = 0;
		free(bufr->filename);
		bufr->filename = NULL;
		setStatusMessage(msg_invalid_utf8);
		free(iopath);
		return -1;
	}

	bufr->dirty = 0;
	if (access(iopath, W_OK) != 0) {
		bufr->read_only = 1;
	}

	int lock_result = lockFile(bufr, iopath);
	if (lock_result == -1) {
		bufr->read_only = 1;
	}

	free(iopath);

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

	if (lock_result != -1)
		setStatusMessage(msg_lines_columns, bufr->numrows, max_width);
	return 0;
}

void revert(void) {
	struct buffer *buf = E.buf;
	struct buffer *new = newBuffer();
	if (editorOpen(new, buf->filename) < 0) {
		/* Open/validation failed — keep the current buffer */
		destroyBuffer(new);
		return;
	}
	new->next = buf->next;
	E.buf = new;
	if (E.headbuf == buf) {
		E.headbuf = new;
	}
	struct buffer *cur = E.headbuf;
	while (cur != NULL) {
		if (cur->next == buf) {
			cur->next = new;
			break;
		}
		cur = cur->next;
	}
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->buf == buf) {
			E.windows[i]->buf = new;
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

void save(void) {
	/* Not allowed during macro record/playback */
	if (E.recording || E.playback) {
		setStatusMessage(msg_macro_blocked);
		return;
	}
	if (E.buf->filename == NULL) {
		char *input = (char *)editorPrompt(
			E.buf, (uint8_t *)"Save as: %s", PROMPT_FILES, NULL);
		if (input == NULL) {
			setStatusMessage(msg_save_aborted);
			return;
		}
		E.buf->filename = collapseHome(input);
		free(input);
		E.buf->special_buffer = 0;
		computeDisplayNames();
	}

	/* Resolve to an OS-usable path for all I/O */
	char *iopath = expandTilde(E.buf->filename);

	size_t len;
	char *buf = rowsToString(E.buf, &len);

	/* Build temp filename: <iopath>.tmpXXXXXX */
	char tmpname[PATH_MAX];
	snprintf(tmpname, sizeof(tmpname), "%s.tmpXXXXXX", iopath);

	int fd = mkstemp(tmpname);
	if (fd == -1) {
		free(buf);
		free(iopath);
		setStatusMessage(msg_save_failed, strerror(errno));
		return;
	}

	/* Preserve permissions if file already exists */
	struct stat st;
	if (stat(iopath, &st) == 0) {
		fchmod(fd, st.st_mode);
	}

	/* Write buffer fully, handling partial writes and EINTR */
	size_t total = 0;
	while (total < len) {
		ssize_t n = write(fd, buf + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmpname);
			free(buf);
			free(iopath);
			setStatusMessage(msg_save_failed, strerror(errno));
			return;
		} else if (n == 0) {
			close(fd);
			unlink(tmpname);
			free(buf);
			free(iopath);
			setStatusMessage(msg_save_failed);
			return;
		}
		total += (size_t)n;
	}

	if (fsync(fd) == -1) {
		close(fd);
		unlink(tmpname);
		free(buf);
		free(iopath);
		setStatusMessage(msg_save_failed, strerror(errno));
		return;
	}

	close(fd);

	if (rename(tmpname, iopath) == -1) {
		unlink(tmpname);
		free(buf);
		free(iopath);
		setStatusMessage(msg_save_failed, strerror(errno));
		return;
	}

	free(buf);
	E.buf->dirty = 0;

	for (int i = 0; i < E.buf->numrows; i++) {
		erow *row = &E.buf->row[i];
		if (row->charcap > row->size + 1) {
			row->chars = xrealloc(row->chars, row->size + 1);
			row->charcap = row->size + 1;
		}
	}

	/* Update stored mtime after save */
	struct stat save_st;
	if (stat(iopath, &save_st) == 0)
		E.buf->open_mtime = save_st.st_mtime;
	E.buf->external_mod = 0;
	E.buf->internal_mod = 1;

	if (E.buf->lock_fd < 0)
		lockFile(E.buf, iopath);

	free(iopath);

	int n = snprintf(NULL, 0, msg_wrote_bytes, (int)len, E.buf->filename);
	char *showName =
		leftTruncate(E.buf->filename, nameFit(E.buf->filename, n));
	setStatusMessage(msg_wrote_bytes, (int)len, showName);
	free(showName);
}

void saveAs(void) {
	/* Not allowed during macro record/playback */
	if (E.recording || E.playback) {
		setStatusMessage(msg_macro_blocked);
		return;
	}
	char *new_filename = (char *)editorPrompt(
		E.buf, (uint8_t *)"Save as: %s", PROMPT_FILES, NULL);
	if (new_filename == NULL) {
		setStatusMessage(msg_save_aborted);
		return;
	}
	free(E.buf->filename);
	E.buf->filename = collapseHome(new_filename);
	free(new_filename);
	/* Release lock on the old file before saving to the new one */
	releaseLock(E.buf);
	computeDisplayNames();
	save();
}

/* Switch the focused window to the named file.  If a buffer with that
 * filename already exists, reuse it; otherwise open a new one.
 * Returns the buffer on success, NULL on failure. */
struct buffer *switchToFile(const char *filename) {
	/* Check if already open */
	struct buffer *buf = findBufferByName(filename);
	if (buf) {
		E.buf = buf;
		E.windows[windowFocusedIdx()]->buf = buf;
		return buf;
	}

	/* Open new buffer */
	struct buffer *nb = newBuffer();
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

/* Check whether a filename contains glob wildcard characters. */
static int hasGlobChars(const char *s) {
	for (; *s; s++) {
		if (*s == '*' || *s == '?' || *s == '[')
			return 1;
	}
	return 0;
}

void findFile(void) {
	/* Not allowed during macro record/playback */
	if (E.recording || E.playback) {
		setStatusMessage(msg_macro_blocked);
		return;
	}
	uint8_t *prompt =
		editorPrompt(E.buf, msg_find_file, PROMPT_FILES, NULL);

	if (prompt == NULL) {
		setStatusMessage(msg_canceled);
		return;
	}

	/* If the input contains glob wildcards, expand and open all matches */
	if (hasGlobChars((char *)prompt)) {
		/* Expand ~ for glob — OS doesn't understand tilde */
		char *glob_input = expandTilde((char *)prompt);
		glob_t gl;
		int rc = glob(glob_input, GLOB_MARK, NULL, &gl);
		free(glob_input);
		if (rc != 0 || gl.gl_pathc == 0) {
			if (rc == 0)
				globfree(&gl);
			setStatusMessage(msg_no_glob_match, prompt);
			free(prompt);
			return;
		}

		struct buffer *last = NULL;
		int opened = 0;
		for (size_t i = 0; i < gl.gl_pathc; i++) {
			/* Skip directories (GLOB_MARK appends '/') */
			size_t plen = strlen(gl.gl_pathv[i]);
			if (plen > 0 && gl.gl_pathv[i][plen - 1] == '/')
				continue;
			struct buffer *buf = switchToFile(gl.gl_pathv[i]);
			if (buf) {
				last = buf;
				opened++;
			}
		}
		globfree(&gl);
		free(prompt);

		if (last) {
			E.buf = last;
			E.windows[windowFocusedIdx()]->buf = last;
			computeDisplayNames();
			refreshScreen();
		}
		if (opened > 1)
			setStatusMessage("Opened %d files", opened);
		return;
	}

	/* Safety net: if a directory path somehow gets through the prompt,
	 * don't try to open it as a file. */
	struct stat st;
	char *stat_path = expandTilde((char *)prompt);
	if (stat(stat_path, &st) == 0 && S_ISDIR(st.st_mode)) {
		setStatusMessage(msg_dir_not_supported);
		free(stat_path);
		free(prompt);
		return;
	}
	free(stat_path);

	struct buffer *buf = switchToFile((char *)prompt);
	computeDisplayNames();
	free(prompt);
	if (buf)
		refreshScreen();
}

void insertFile(void) {
	struct buffer *buf = E.buf;
	uint8_t *filename =
		editorPrompt(buf, "Insert file: %s", PROMPT_FILES, NULL);
	if (filename == NULL) {
		return;
	}

	char *iopath = expandTilde((char *)filename);

	/* Reject directories and check file size against budget */
	struct stat ist;
	if (stat(iopath, &ist) == 0) {
		if (S_ISDIR(ist.st_mode)) {
			setStatusMessage(msg_dir_not_supported);
			free(iopath);
			free(filename);
			return;
		}
		if (S_ISREG(ist.st_mode) &&
		    totalOpenBytes() + totalKillBytes() + (size_t)ist.st_size >
			    (size_t)EMIL_MAX_OPEN_BYTES) {
			setStatusMessage(msg_memory_limit);
			free(iopath);
			free(filename);
			return;
		}
	}

	FILE *fp = fopen(iopath, "r");
	free(iopath);
	if (!fp) {
		if (errno == ENOENT) {
			/* Use filename (not iopath) for display */
			int n = snprintf(NULL, 0, msg_file_not_found,
					 (char *)filename);
			char *showName = leftTruncate(
				(char *)filename, nameFit((char *)filename, n));
			setStatusMessage(msg_file_not_found, showName);
			free(showName);
		} else {
			setStatusMessage(msg_error_opening, strerror(errno));
		}
		free(filename);
		return;
	}

	/* Pre-scan for null bytes */
	if (fileContainsNullBytes(fp)) {
		fclose(fp);
		setStatusMessage(msg_binary_file);
		free(filename);
		return;
	}

	/* Load into a temporary buffer so we can validate before
	 * modifying the real buffer */
	struct buffer *tmpbuf = newBuffer();

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while ((linelen = emil_getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
				       line[linelen - 1] == '\r')) {
			linelen--;
		}
		insertRow(tmpbuf, tmpbuf->numrows, line, linelen);
	}

	free(line);
	fclose(fp);

	/* Validate UTF-8 before inserting */
	if (!checkUTF8Validity(tmpbuf)) {
		destroyBuffer(tmpbuf);
		setStatusMessage(msg_invalid_utf8);
		free(filename);
		return;
	}

	/* Now insert the validated content into the actual buffer */
	int saved_cy = buf->cy;
	int lines_inserted = 0;

	for (int i = 0; i < tmpbuf->numrows; i++) {
		insertRow(buf, saved_cy + lines_inserted,
			  (char *)tmpbuf->row[i].chars, tmpbuf->row[i].size);
		lines_inserted++;
	}

	destroyBuffer(tmpbuf);

	if (lines_inserted > 0) {
		buf->cy = saved_cy + lines_inserted - 1;
		buf->cx = buf->row[buf->cy].size;
	}

	int n = snprintf(NULL, 0, msg_inserted_lines, lines_inserted,
			 (char *)filename);
	char *showName =
		leftTruncate((char *)filename, nameFit((char *)filename, n));
	setStatusMessage(msg_inserted_lines, lines_inserted, showName);
	free(showName);
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
char *cleanPath(char *path) {
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
 * Used by changeDirectory and exposed for testing. */
char *rebaseFilename(const char *filename, const char *old_cwd,
		     const char *new_cwd) {
	/* Absolute and ~-prefixed paths are location-independent */
	if (filename[0] == '/')
		return xstrdup(filename);
	if (filename[0] == '~' && (filename[1] == '\0' || filename[1] == '/'))
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

void changeDirectory(void) {
	uint8_t *dir = editorPrompt(E.buf, (uint8_t *)"Directory: %s",
				    PROMPT_DIR, NULL);
	if (dir == NULL) {
		setStatusMessage(msg_canceled);
		return;
	}

	/* Grab the old cwd before changing */
	char old_cwd[PATH_MAX];
	if (getcwd(old_cwd, sizeof(old_cwd)) == NULL) {
		setStatusMessage(msg_indeterminate_cd);
		free(dir);
		return;
	}

	char *iodir = expandTilde((char *)dir);
	if (chdir(iodir) != 0) {
		setStatusMessage("cd: %s: %s", (char *)dir, strerror(errno));
		free(iodir);
		free(dir);
		return;
	}
	free(iodir);

	char new_cwd[PATH_MAX];
	if (getcwd(new_cwd, sizeof(new_cwd)) == NULL) {
		/* chdir succeeded but getcwd failed — unlikely but
		 * leave filenames as-is */
		setStatusMessage(msg_changed_dir);
		free(dir);
		return;
	}

	/* If the directory actually changed, update relative filenames */
	if (strcmp(old_cwd, new_cwd) != 0) {
		for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
			if (b->filename == NULL || b->special_buffer)
				continue;
			char *new_name =
				rebaseFilename(b->filename, old_cwd, new_cwd);
			free(b->filename);
			b->filename = new_name;
		}

		computeDisplayNames();
	}

	setStatusMessage(msg_current_dir, new_cwd);
	free(dir);
}

/*** stdin loading ***/

/*
 * Read all available data from a file descriptor into a malloc'd buffer.
 * Sets *out_len to the number of bytes read.  Returns NULL on allocation
 * failure; returns an empty buffer (out_len == 0) if nothing was read.
 */
char *readAllFromFd(int fd, size_t *out_len) {
	size_t cap = BUFSIZ;
	size_t len = 0;
	char *buf = xmalloc(cap);
	ssize_t n;
	while ((n = read(fd, buf + len, cap - len)) > 0) {
		len += (size_t)n;
		if (len >= cap) {
			cap <<= 1;
			buf = xrealloc(buf, cap);
		}
	}
	*out_len = len;
	return buf;
}

/*
 * Load piped stdin data into a new editor buffer.  The data is split
 * on newline boundaries and inserted row by row, matching the same
 * approach used by editorOpen().  The buffer is named "*stdin*" and
 * marked read-only.
 *
 * Returns the new buffer, or NULL if the data contains null bytes
 * (which would indicate binary / non-UTF-8 content).
 */
struct buffer *loadStdinBuffer(const char *data, size_t len) {
	/* Reject binary data: null bytes can't be represented */
	if (memchr(data, '\0', len) != NULL) {
		return NULL;
	}

	struct buffer *buf = newBuffer();
	buf->filename = xstrdup("*stdin*");

	size_t start = 0;
	for (size_t i = 0; i < len; i++) {
		if (data[i] == '\n') {
			/* Strip trailing \r for DOS line endings */
			size_t end = i;
			if (end > start && data[end - 1] == '\r')
				end--;
			insertRow(buf, buf->numrows, (char *)&data[start],
				  (int)(end - start));
			start = i + 1;
		}
	}
	/* Handle final line without trailing newline */
	if (start < len) {
		size_t end = len;
		if (end > start && data[end - 1] == '\r')
			end--;
		insertRow(buf, buf->numrows, (char *)&data[start],
			  (int)(end - start));
	}

	buf->dirty = 0;
	buf->read_only = 1;
	buf->word_wrap = 1;
	return buf;
}
