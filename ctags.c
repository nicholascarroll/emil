/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include "ctags.h"
#include "emil.h"

#include "buffer.h"
#include "dbuf.h"
#include "fileio.h"
#include "display.h"
#include "keymap.h"
#include "motion.h"
#include "terminal.h"
#include "window.h"
#include "util.h"
#include "unicode.h"

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

/* Non-ASCII characters that end a word: the Latin-1 punctuation block
 * (NBSP, guillemets, inverted marks), General Punctuation (curly
 * quotes, dashes, ellipsis, and the zero-width characters), CJK
 * punctuation, and ZERO WIDTH NO-BREAK SPACE.  No script is named.
 * Where a script is written without spaces, the text marks its own
 * word boundaries with U+200B ZERO WIDTH SPACE; deciding where they go
 * is the job of whatever inserted them, not of the editor. */
static int isSeparatorCP(uint32_t cp) {
	return (cp >= 0x00A0 && cp <= 0x00BF) ||
	       (cp >= 0x2000 && cp <= 0x206F) ||
	       (cp >= 0x3000 && cp <= 0x303F) || cp == 0xFEFF;
}

static int isZeroWidth(uint32_t cp) {
	return cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0xFEFF;
}

/* A word is a run of ASCII identifier characters and non-ASCII
 * characters that are not separators, so "café", "Ελλάδα" and a
 * ZWSP-delimited Thai word are each one word. */
static int isWordAt(const erow *row, int i) {
	if (i < 0 || i >= row->size)
		return 0;
	uint8_t c = row->chars[i];
	if (c < 0x80)
		return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		       (c >= '0' && c <= '9') || c == '_';
	return !isSeparatorCP(utf8Decode(row->chars, i));
}

/* Byte offset of the codepoint that ends just before 'i'. */
static int prevCPStart(const erow *row, int i) {
	do {
		i--;
	} while (i > 0 && utf8_isCont(row->chars[i]));
	return i;
}

/* Byte offset of the codepoint after the one at 'i'.  Counts the
 * continuation bytes actually present rather than trusting the lead
 * byte, so a truncated sequence cannot swallow the bytes after it. */
static int nextCPStart(const erow *row, int i) {
	int want = utf8_nBytes(row->chars[i]);
	int n = 1;
	while (n < want && i + n < row->size && utf8_isCont(row->chars[i + n]))
		n++;
	return i + n;
}

char *ctagsWordAtPoint(void) {
	erow *row = &E.buf->row[E.buf->cy];
	int cx = E.buf->cx;

	/* A zero-width separator takes a byte offset but no screen cell,
	 * so the cursor is drawn on the character after it.  Look up
	 * that character's word, if it has one; otherwise fall back from
	 * where the cursor really is, so a separator at the end of a
	 * word still finds the word. */
	int orig = cx;
	while (cx < row->size && row->chars[cx] >= 0x80 &&
	       isZeroWidth(utf8Decode(row->chars, cx)))
		cx = nextCPStart(row, cx);
	if (!isWordAt(row, cx))
		cx = orig;

	/* On a word, or just after one. */
	if (!isWordAt(row, cx)) {
		if (cx > 0 && isWordAt(row, prevCPStart(row, cx)))
			cx = prevCPStart(row, cx);
		else
			return NULL;
	}
	int start = cx, end = cx;
	while (start > 0 && isWordAt(row, prevCPStart(row, start)))
		start = prevCPStart(row, start);
	while (end < row->size && isWordAt(row, end))
		end = nextCPStart(row, end);

	char *w = xmalloc(end - start + 1);
	memcpy(w, &row->chars[start], end - start);
	w[end - start] = '\0';
	return w;
}

/* ---- tags file lookup ---- */

/* Walk from the current working directory up toward filesystem root.
 *  The depth bound keeps a pathological filesystem from spinning forever. */
int findTagsDir(char *out_dir, size_t dirsz) {
	char dir[PATH_MAX];
	if (getcwd(dir, sizeof(dir)) == NULL)
		return -1;

	for (int depth = 0; depth < 256; depth++) {
		char path[PATH_MAX];
		int n = snprintf(path, sizeof(path), "%s/tags", dir);
		if (n < 0 || (size_t)n >= sizeof(path)) {
			/* Path too long to probe; stop rather than
			 * risk a truncated access() of the wrong file. */
			return -1;
		}

		if (access(path, R_OK) == 0) {
			if (emil_strlcpy(out_dir, dir, dirsz) >= dirsz)
				return -1;
			return 0;
		}

		/* Not here: step up to the parent directory. */
		if (dir[0] == '/' && dir[1] == '\0')
			break; /* already at root */
		char *slash = strrchr(dir, '/');
		if (slash == NULL)
			break; /* getcwd is absolute; shouldn't happen */
		if (slash == dir)
			dir[1] = '\0'; /* parent is root "/" */
		else
			*slash = '\0';
	}

	return -1;
}

int resolveTagPath(const char *tagsdir, const char *tagpath, char *out,
		   size_t outsz) {
	int n;
	if (tagpath[0] == '/' || tagpath[0] == '~')
		n = snprintf(out, outsz, "%s", tagpath);
	else
		n = snprintf(out, outsz, "%s/%s", tagsdir, tagpath);
	if (n < 0 || (size_t)n >= outsz)
		return -1;
	return 0;
}

/* Open the project's tags file (found via findTagsDir) and report the
 * directory it lives in.  Returns NULL if none is found. */
static FILE *openTagsFile(char *out_dir, size_t dirsz) {
	if (findTagsDir(out_dir, dirsz) != 0)
		return NULL;

	char path[PATH_MAX];
	int n = snprintf(path, sizeof(path), "%s/tags", out_dir);
	if (n < 0 || (size_t)n >= sizeof(path))
		return NULL;
	return fopen(path, "r");
}

/* Line number from a tags file, or 0 if it is not a usable one. */
static int tagLineNumber(const char *s, char **endp) {
	char *e;
	long n = strtol(s, &e, 10);
	if (endp)
		*endp = e;
	return (n > 0 && n <= INT_MAX) ? (int)n : 0;
}

/* Keys of the extension fields that name the scope a tag is defined
 * in: Universal Ctags writes "class:StreamWriter" on a Python method
 * and "struct:buffer" on a C member. */
static int isScopeKey(const char *key, size_t len) {
	static const char *const keys[] = {
		"class",     "struct", "union",	    "enum",
		"namespace", "module", "interface", "implementation",
		"function",  "method", "package",
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++)
		if (strlen(keys[i]) == len && memcmp(keys[i], key, len) == 0)
			return 1;
	return 0;
}

static void tagField(struct tagMatch *m, char *field) {
	char *colon = strchr(field, ':');
	if (!colon)
		return; /* a bare kind letter */
	size_t klen = (size_t)(colon - field);
	char *val = colon + 1;
	if (klen == 4 && memcmp(field, "line", 4) == 0) {
		if (!m->line)
			m->line = tagLineNumber(val, NULL);
	} else if (klen == 5 && memcmp(field, "scope", 5) == 0) {
		/* --fields=+Z writes "scope:class:Foo" */
		char *name = strchr(val, ':');
		m->scope = name ? name + 1 : val;
	} else if (!m->scope && isScopeKey(field, klen)) {
		m->scope = val;
	}
}

int ctagsParseLine(char *line, const char *sym, struct tagMatch *m) {
	static char empty[1];
	size_t symlen = strlen(sym);
	if (line[0] == '!' || strncmp(line, sym, symlen) != 0 ||
	    line[symlen] != '\t')
		return -1;
	line[strcspn(line, "\r\n")] = '\0';
	char *p = strchr(line + symlen + 1, '\t');
	if (!p)
		return -1;
	*p++ = '\0';
	m->file = line + symlen + 1;
	m->path = NULL;
	m->pat = empty;
	m->scope = NULL;
	m->line = 0;

	/* The address: a line number, a /pattern/ or ?pattern?, or both
	 * joined by ';' as ctags --excmd=combine writes them. */
	if (*p >= '0' && *p <= '9') {
		m->line = tagLineNumber(p, &p);
		if (p[0] == ';' && (p[1] == '/' || p[1] == '?'))
			p++;
	}
	if (*p == '/' || *p == '?') {
		/* Strip the delimiters and the ^...$ anchors, and undo the
		 * escaping of the delimiters and of backslash. */
		char delim = *p++;
		if (*p == '^')
			p++;
		char *out = m->pat = p;
		while (*p && *p != delim) {
			if (p[0] == '\\' &&
			    (p[1] == '/' || p[1] == '?' || p[1] == '\\'))
				p++;
			*out++ = *p++;
		}
		if (*p == delim) {
			p++;
			if (out > m->pat && out[-1] == '$')
				out--;
		}
		*out = '\0';
	}

	/* Extension fields: ;" then tab-separated key:value pairs. */
	if (p[0] == ';' && p[1] == '"') {
		char *f = p + 2;
		int more = (*f == '\t');
		while (more) {
			char *field = f + 1;
			f = field + strcspn(field, "\t");
			more = (*f == '\t');
			*f = '\0';
			tagField(m, field);
		}
	}
	return 0;
}

static void freeMatches(struct tagMatch *v, int n) {
	for (int i = 0; i < n; i++) {
		free(v[i].file);
		free(v[i].path);
		free(v[i].pat);
		free(v[i].scope);
	}
	free(v);
}

/* Move the entries in the file being read to the front, keeping the
 * order within each group: the definition next to the reader is the
 * likeliest one.  findBufferForFile() matches a path that names the
 * file by another route, such as a symlink. */
static void currentFileFirst(struct tagMatch *v, int n) {
	if (!E.buf->filename || E.buf->special_buffer)
		return;
	int front = 0;
	for (int i = 0; i < n; i++) {
		if (findBufferForFile(v[i].path, NULL) == E.buf) {
			struct tagMatch t = v[i];
			memmove(&v[front + 1], &v[front],
				(size_t)(i - front) * sizeof(*v));
			v[front++] = t;
		}
	}
}

/* Every entry for sym in the project's tags file, those in the current
 * buffer's file first and the rest in tags-file order.  Returns the
 * count and sets *out (NULL when there are none), or -1 if there is no
 * tags file.  Lines are read whole, however long. */
static int collectMatches(const char *sym, struct tagMatch **out) {
	char dir[PATH_MAX];
	*out = NULL;
	FILE *fp = openTagsFile(dir, sizeof(dir));
	if (!fp)
		return -1;

	struct tagMatch *v = NULL;
	int n = 0, cap = 0;
	char *line = NULL;
	size_t linecap = 0;
	while (emil_getline(&line, &linecap, fp) != -1) {
		struct tagMatch t;
		char path[PATH_MAX];
		if (ctagsParseLine(line, sym, &t) != 0 ||
		    resolveTagPath(dir, t.file, path, sizeof(path)) != 0)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			v = xrealloc(v, (size_t)cap * sizeof(*v));
		}
		v[n].file = xstrdup(t.file);
		v[n].path = xstrdup(path);
		v[n].pat = xstrdup(t.pat);
		v[n].scope = t.scope ? xstrdup(t.scope) : NULL;
		v[n].line = t.line;
		n++;
	}
	free(line);
	fclose(fp);
	if (n > 1)
		currentFileFirst(v, n);
	*out = v;
	return n;
}

/* ---- disambiguation menu ---- */

#define TAGS_MENU_NAME "*Tags*"
#define TAGS_SCOPE_COLS_MAX 30

/* One row: the scope padded to a column, the file and line, then the
 * definition's own text.  The name is the same on every row, so it is
 * in the status line instead. */
static void menuRow(struct dbuf *d, const struct tagMatch *m, int scope_cols) {
	if (scope_cols > 0) {
		const char *s = m->scope ? m->scope : "";
		dbuf_append(d, (const uint8_t *)s, (int)strlen(s));
		dbuf_pad(d, ' ', scope_cols - stringWidth((const uint8_t *)s));
		dbuf_append(d, (const uint8_t *)"  ", 2);
	}
	dbuf_append(d, (const uint8_t *)m->file, (int)strlen(m->file));
	if (m->line > 0) {
		char num[16];
		int len = snprintf(num, sizeof(num), ":%d", m->line);
		dbuf_append(d, (const uint8_t *)num, len);
	}
	const char *def = m->pat;
	while (*def == ' ' || *def == '\t')
		def++;
	if (*def) {
		dbuf_append(d, (const uint8_t *)"  ", 2);
		dbuf_append(d, (const uint8_t *)def, (int)strlen(def));
	}
}

/* Let the reader choose one of n matches.  The list opens in a popup
 * while focus stays on the window being read, as the completion list
 * does during a prompt, and the popup's region marks the selection, so
 * nothing here moves E.buf.  Returns the chosen index, or -1 on C-g. */
static int pickMatch(const char *sym, const struct tagMatch *v, int n) {
	int scope_cols = 0;
	for (int i = 0; i < n; i++) {
		int w = v[i].scope ? stringWidth((const uint8_t *)v[i].scope) :
				     0;
		if (w > scope_cols)
			scope_cols = w;
	}
	if (scope_cols > TAGS_SCOPE_COLS_MAX)
		scope_cols = TAGS_SCOPE_COLS_MAX;

	struct buffer *pb = findOrCreateSpecialBuffer(TAGS_MENU_NAME);
	bufferResetRows(pb);
	for (int i = 0; i < n; i++) {
		struct dbuf d = DBUF_INIT;
		int len;
		menuRow(&d, &v[i], scope_cols);
		uint8_t *row = dbuf_detach(&d, &len);
		insertRow(pb, i, row, (size_t)len);
		free(row);
	}
	pb->read_only = 1;
	pb->word_wrap = 0;
	pb->rectangle_mode = 0;
	pb->mark_active = 1;
	showPopupBuffer(pb);
	setStatusMessage("%s: %d definitions.  RET visits, C-g cancels", sym,
			 n);

	int sel = 0;
	for (;;) {
		/* The region spans the selected row. */
		pb->cy = pb->marky = sel;
		pb->markx = 0;
		pb->cx = pb->row[sel].size;
		int page = 1;
		int win = findBufferWindow(pb);
		if (win >= 0) {
			scrollToShowCursor(E.windows[win], pb);
			page = E.windows[win]->height - page_overlap;
			if (page < 1)
				page = 1;
		}
		refreshScreen();

		int key = readKey();
		if (key == -1)
			continue;
		recordKey(key);
		if (key == '\r' || key == CTRL('j'))
			break;
		if (key == CTRL('g')) {
			sel = -1;
			break;
		}
		switch (resolveBinding(key)) {
		case CMD_NEXT_LINE:
		case CMD_SCROLL_DOWN:
			sel++;
			break;
		case CMD_PREV_LINE:
		case CMD_SCROLL_UP:
			sel--;
			break;
		case CMD_PAGE_DOWN:
			sel += page;
			break;
		case CMD_PAGE_UP:
			sel -= page;
			break;
		case CMD_BEG_OF_FILE:
			sel = 0;
			break;
		case CMD_END_OF_FILE:
			sel = n - 1;
			break;
		default:
			break;
		}
		if (sel < 0)
			sel = 0;
		if (sel > n - 1)
			sel = n - 1;
	}
	closeSpecialBuffer(TAGS_MENU_NAME);
	return sel;
}

/* ---- public API ---- */

static void visitMatch(const struct tagMatch *m, const char *sym) {
	pushLocation();
	struct buffer *buf = switchToFile(m->path);
	if (buf) {
		if (m->line > 0) {
			buf->cy = (m->line - 1 < buf->numrows) ? m->line - 1 :
								 0;
			buf->cx = 0;
		} else if (m->pat[0]) {
			size_t plen = strlen(m->pat);
			for (int r = 0; r < buf->numrows; r++) {
				if ((size_t)buf->row[r].size >= plen &&
				    memcmp(buf->row[r].chars, m->pat, plen) ==
					    0) {
					buf->cy = r;
					buf->cx = 0;
					break;
				}
			}
		}
		recenter(E.windows[windowFocusedIdx()]);
	}
	setStatusMessage("Tag: %s", sym);
}

void ctagsJump(void) {
	char *sym = ctagsWordAtPoint();
	if (!sym) {
		setStatusMessage("No symbol at point");
		return;
	}
	struct tagMatch *v;
	int n = collectMatches(sym, &v);
	int pick = n > 1 ? pickMatch(sym, v, n) : 0;
	if (n <= 0)
		setStatusMessage("Tag not found: %s", sym);
	else if (pick < 0)
		setStatusMessage("Canceled.");
	else
		visitMatch(&v[pick], sym);
	freeMatches(v, n > 0 ? n : 0);
	free(sym);
}

void ctagsBack(void) {
	if (jsp == 0) {
		setStatusMessage("Tag stack empty");
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
		setStatusMessage("No file extension");
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
		setStatusMessage("No header/body mapping for %s", ext);
		return;
	}

	char other[PATH_MAX];
	/* base_len is measured on the original string; if the filename
	 * doesn't fit in 'other', the offset arithmetic below would
	 * write past the buffer */
	if (strlen(E.buf->filename) >= sizeof(other)) {
		setStatusMessage("No header/body mapping for %s", ext);
		return;
	}
	emil_strlcpy(other, E.buf->filename, sizeof(other));

	size_t base_len = ext - E.buf->filename;
	snprintf(other + base_len, sizeof(other) - base_len, "%s", target_ext);

	char *ioother = expandTilde(other);
	if (access(ioother, F_OK) != 0) {
		setStatusMessage("No counterpart file: %s", other);
		free(ioother);
		return;
	}
	free(ioother);

	pushLocation();
	switchToFile(other);
}
