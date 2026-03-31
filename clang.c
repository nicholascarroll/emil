/* clang.c — ctags jump (M-.), jump back (M-,), toggle .c/.h (M-/) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include "clang.h"
#include "emil.h"
#include "message.h"
#include "fileio.h"
#include "display.h"
#include "window.h"
#include "util.h"

extern struct config E;

/* ---- jump-back stack ---- */

#define CTAGS_STACK_SIZE 32

static struct {
	char *filename;
	int cx, cy;
} jstack[CTAGS_STACK_SIZE];
static int jsp;

static void pushLocation(void) {
	const char *fn = E.buf->filename ? E.buf->filename : "*scratch*";
	if (jsp >= CTAGS_STACK_SIZE) {
		free(jstack[0].filename);
		memmove(&jstack[0], &jstack[1],
			(CTAGS_STACK_SIZE - 1) * sizeof(jstack[0]));
		jsp = CTAGS_STACK_SIZE - 1;
	}
	jstack[jsp].filename = xstrdup(fn);
	jstack[jsp].cx = E.buf->cx;
	jstack[jsp].cy = E.buf->cy;
	jsp++;
}

/* ---- word at point ---- */

static int isIdentChar(uint8_t c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
	       (c >= '0' && c <= '9') || c == '_';
}

static char *wordAtPoint(void) {
	if (E.buf->cy >= E.buf->numrows)
		return NULL;
	erow *row = &E.buf->row[E.buf->cy];
	int cx = E.buf->cx;
	if (cx >= row->size || !isIdentChar(row->chars[cx])) {
		if (cx > 0 && isIdentChar(row->chars[cx - 1]))
			cx--;
		else
			return NULL;
	}
	int start = cx, end = cx;
	while (start > 0 && isIdentChar(row->chars[start - 1]))
		start--;
	while (end < row->size && isIdentChar(row->chars[end]))
		end++;
	char *w = xmalloc(end - start + 1);
	memcpy(w, &row->chars[start], end - start);
	w[end - start] = '\0';
	return w;
}

/* ---- ctags file lookup (prefers .c over .h) ---- */

static int ctagsLookup(const char *sym, char *out_file, size_t filesz,
		       int *out_line, char *out_pat, size_t patsz) {
	FILE *fp = fopen("tags", "r");
	if (!fp)
		return -1;

	char line[1024];
	size_t symlen = strlen(sym);
	int found = 0;

	while (fgets(line, sizeof(line), fp)) {
		if (line[0] == '!' || strncmp(line, sym, symlen) != 0 ||
		    line[symlen] != '\t')
			continue;

		char *fstart = &line[symlen] + 1;
		char *tab2 = strchr(fstart, '\t');
		if (!tab2)
			continue;

		size_t flen = (size_t)(tab2 - fstart);
		int is_c = (flen >= 2 && fstart[flen - 2] == '.' &&
			    fstart[flen - 1] == 'c');

		if (found && !is_c)
			continue;

		if (flen >= filesz)
			flen = filesz - 1;
		memcpy(out_file, fstart, flen);
		out_file[flen] = '\0';

		char *addr = tab2 + 1;
		char *nl = strpbrk(addr, ";\"\r\n");
		if (nl)
			*nl = '\0';

		out_pat[0] = '\0';
		if (addr[0] >= '0' && addr[0] <= '9') {
			*out_line = atoi(addr);
		} else {
			*out_line = 0;
			/* POSIX pattern: /^literal$/ or ?^literal$? */
			char *p = addr;
			char delim = *p;
			if (delim == '/' || delim == '?')
				p++;
			if (*p == '^')
				p++;
			/* Copy pattern, strip trailing anchor+delim,
			 * unescape \/ and \\ */
			size_t pi = 0;
			while (*p && pi < patsz - 1) {
				if (p[0] == '\\' &&
				    (p[1] == '/' || p[1] == '?' ||
				     p[1] == '\\')) {
					out_pat[pi++] = p[1];
					p += 2;
				} else if ((*p == '$' &&
					    (p[1] == delim || p[1] == '\0')) ||
					   *p == delim) {
					break;
				} else {
					out_pat[pi++] = *p++;
				}
			}
			out_pat[pi] = '\0';
		}

		found = 1;
		if (is_c)
			break;
	}

	fclose(fp);
	return found ? 0 : -1;
}

/* ---- public API ---- */

void ctagsJump(void) {
	char *sym = wordAtPoint();
	if (!sym) {
		setStatusMessage(msg_no_symbol_at_point);
		return;
	}

	char tagfile[PATH_MAX];
	char tagpat[1024];
	int tagline;
	if (ctagsLookup(sym, tagfile, sizeof(tagfile), &tagline, tagpat,
			sizeof(tagpat)) < 0) {
		setStatusMessage(msg_tag_not_found, sym);
		free(sym);
		return;
	}

	pushLocation();
	struct buffer *buf = switchToFile(tagfile);
	if (buf) {
		if (tagline > 0) {
			buf->cy = (tagline - 1 < buf->numrows) ? tagline - 1 :
								 0;
			buf->cx = 0;
		} else if (tagpat[0]) {
			size_t plen = strlen(tagpat);
			for (int r = 0; r < buf->numrows; r++) {
				if ((size_t)buf->row[r].size >= plen &&
				    memcmp(buf->row[r].chars, tagpat, plen) ==
					    0) {
					buf->cy = r;
					buf->cx = 0;
					break;
				}
			}
		}
		recenter(E.windows[windowFocusedIdx()]);
	}
	setStatusMessage(msg_tag, sym);
	free(sym);
}

void ctagsBack(void) {
	if (jsp == 0) {
		setStatusMessage(msg_tag_stack_empty);
		return;
	}
	jsp--;
	struct buffer *buf = switchToFile(jstack[jsp].filename);
	if (buf) {
		buf->cy = jstack[jsp].cy;
		buf->cx = jstack[jsp].cx;
	}
	free(jstack[jsp].filename);
	jstack[jsp].filename = NULL;
}

void toggleHeaderBody(void) {
	if (!E.buf->filename)
		return;

	char *ext = strrchr(E.buf->filename, '.');
	if (!ext) {
		setStatusMessage(msg_no_file_extension);
		return;
	}

	struct {
		const char *a;
		const char *b;
	} pairs[] = {
		{ ".c", ".h" }, { ".cpp", ".hpp" }, { ".cc", ".h" },
		{ ".m", ".h" }, { ".adb", ".ads" }, { ".pkb", ".pks" },
	};

	const char *target_ext = NULL;

	for (size_t i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++) {
		if (strcmp(ext, pairs[i].a) == 0) {
			target_ext = pairs[i].b;
			break;
		}
		if (strcmp(ext, pairs[i].b) == 0) {
			target_ext = pairs[i].a;
			break;
		}
	}

	if (!target_ext) {
		setStatusMessage(msg_no_ext_mapping, ext);
		return;
	}

	char other[PATH_MAX];
	emil_strlcpy(other, E.buf->filename, sizeof(other));

	size_t base_len = ext - E.buf->filename;
	snprintf(other + base_len, sizeof(other) - base_len, "%s", target_ext);

	char *ioother = expandTilde(other);
	if (access(ioother, F_OK) != 0) {
		setStatusMessage(msg_no_ext_file, other);
		free(ioother);
		return;
	}
	free(ioother);

	pushLocation();
	switchToFile(other);
}
