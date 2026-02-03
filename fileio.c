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
			editorSetStatusMessage("(New file)", bufr->filename);
			return 0;
		}
		editorSetStatusMessage("Can't open file: %s", strerror(errno));
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
		editorSetStatusMessage("File failed UTF-8 validation");
		return -1;
	}

	bufr->dirty = 0;
	/* If the file is not writable by us, mark buffer read-only */
	if (access(filename, W_OK) != 0) {
		bufr->read_only = 1;
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

	/* TODO Interactive fallback to direct write if temp creation fails */
	/* TODO: fsync parent dir after rename */

	editorSetStatusMessage("Wrote %d bytes to %s", len, bufr->filename);
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
	editorSave(bufr);
}

void findFile(void) {
	struct editorConfig *E_ptr = &E;
	uint8_t *prompt =
		editorPrompt(E_ptr->buf, "Find File: %s", PROMPT_FILES, NULL);

	if (prompt == NULL) {
		editorSetStatusMessage("Canceled.");
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
		editorSetStatusMessage("File failed UTF-8 validation");
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
