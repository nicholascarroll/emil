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
	int row;
	for (row = 0; row < bufr->numrows; row++) {
		unsigned char *s = (unsigned char *)bufr->row[row].chars;
		int i = 0;
		while (i < bufr->row[row].size) {
			unsigned char c = s[i];

			if (c == 0x00) {
				/* Null bytes not allowed */
				return 0;
			} else if (c <= 0x7F) {
				/* ASCII byte */
				i++;
			} else if ((c & 0xE0) == 0xC0) {
				/* 2-byte sequence */
				if (i + 1 >= bufr->row[row].size ||
				    (s[i + 1] & 0xC0) != 0x80) {
					return 0;
				}
				unsigned int cp = ((c & 0x1F) << 6) |
						  (s[i + 1] & 0x3F);
				if (cp < 0x80) {
					return 0; /* Overlong */
				}
				i += 2;
			} else if ((c & 0xF0) == 0xE0) {
				/* 3-byte sequence */
				if (i + 2 >= bufr->row[row].size ||
				    (s[i + 1] & 0xC0) != 0x80 ||
				    (s[i + 2] & 0xC0) != 0x80) {
					return 0;
				}
				unsigned int cp = ((c & 0x0F) << 12) |
						  ((s[i + 1] & 0x3F) << 6) |
						  (s[i + 2] & 0x3F);
				if (cp < 0x800) {
					return 0; /* Overlong */
				}
				if (cp >= 0xD800 && cp <= 0xDFFF) {
					return 0; /* Surrogate half */
				}
				i += 3;
			} else if ((c & 0xF8) == 0xF0) {
				/* 4-byte sequence */
				if (i + 3 >= bufr->row[row].size ||
				    (s[i + 1] & 0xC0) != 0x80 ||
				    (s[i + 2] & 0xC0) != 0x80 ||
				    (s[i + 3] & 0xC0) != 0x80) {
					return 0;
				}
				unsigned int cp = ((c & 0x07) << 18) |
						  ((s[i + 1] & 0x3F) << 12) |
						  ((s[i + 2] & 0x3F) << 6) |
						  (s[i + 3] & 0x3F);
				if (cp < 0x10000) {
					return 0; /* Overlong */
				}
				if (cp > 0x10FFFF) {
					return 0; /* Above Unicode max */
				}
				i += 4;
			} else {
				/* Invalid starting byte (0x80-0xBF or 0xF8+) */
				return 0;
			}
		}
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

	/* Pre-scan for null bytes before line-based reading, since
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

void findFile(void) {
	struct editorConfig *E_ptr = &E;
	uint8_t *prompt =
		editorPrompt(E_ptr->buf, "Find File: %s", PROMPT_FILES, NULL);

	if (prompt == NULL) {
		editorSetStatusMessage(msg_canceled);
		return;
	}

	if (prompt[strlen(prompt) - 1] == '/') {
		editorSetStatusMessage("Directory editing not supported.");
		free(prompt);
		return;
	}

	// Check if a buffer with the same filename already exists
	struct editorBuffer *buf = E_ptr->headbuf;
	while (buf != NULL) {
		if (buf->filename != NULL &&
		    strcmp(buf->filename, (char *)prompt) == 0) {
			editorSetStatusMessage(
				"File '%s' already open in a buffer.", prompt);
			free(prompt);
			E_ptr->buf = buf; // Switch to the existing buffer

			// Update the focused window to display the found buffer
			int idx = windowFocusedIdx();
			E_ptr->windows[idx]->buf = E_ptr->buf;

			refreshScreen(); // Refresh to reflect the change
			return;
		}
		buf = buf->next;
	}

	// Create new buffer for the file
	struct editorBuffer *newBuf = newBuffer();
	if (editorOpen(newBuf, (char *)prompt) < 0) {
		/* Validation failed — discard the buffer */
		destroyBuffer(newBuf);
		free(prompt);
		return;
	}
	free(prompt);

	newBuf->next = E_ptr->headbuf;
	E_ptr->headbuf = newBuf;
	E_ptr->buf = newBuf;
	int idx = windowFocusedIdx();
	E_ptr->windows[idx]->buf = E_ptr->buf;
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

void editorChangeDirectory(struct editorConfig *ed, struct editorBuffer *buf) {
	(void)buf; /* unused parameter */

	uint8_t *dir = editorPrompt(ed->buf, (uint8_t *)"Directory: %s",
				    PROMPT_DIR, NULL);
	if (dir == NULL) {
		editorSetStatusMessage(msg_canceled);
		return;
	}

	if (chdir((char *)dir) == 0) {
		char cwd[PATH_MAX];
		if (getcwd(cwd, sizeof(cwd))) {
			editorSetStatusMessage("Current directory: %s", cwd);
		} else {
			editorSetStatusMessage("Changed directory");
		}
	} else {
		editorSetStatusMessage("cd: %s: %s", (char *)dir,
				       strerror(errno));
	}

	free(dir);
}
