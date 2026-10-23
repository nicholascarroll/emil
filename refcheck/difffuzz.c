/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* difffuzz.c: refcheck's differential keystroke driver.
 *
 * Derived from tests/fuzz_keys.c (same key generator, same readKey
 * replacement, same per-sequence setup).  It adds one mode:
 *
 *   difffuzz trace SEED [SKIPFILE]
 *
 * which runs one sequence in-process and, after every key, writes one
 * line of state digests to stdout's replacement, fd 3:
 *
 *   <index> buf=<h> cur=<h> mark=<h> status=<h> kill=<h> reg=<h>
 *           win=<h> files=<h> lead=<h>
 *
 * Each field is an FNV-1a hash of one part of the editor's observable
 * state, so a divergence between two builds names the parts that
 * differ.  The same driver source is compiled against the old and the
 * new commit's headers and objects; refcheck/difffuzz.py runs both on
 * the same seed and compares the lines.
 *
 * Screen output still goes to /dev/null: comparing rendered screens
 * needs a terminal emulator and is not in this version.
 *
 * The run/replay/show/min modes of fuzz_keys are kept for crash hunting.
 */

#include "emil.h"
#include "buffer.h"
#include "display.h"
#include "fileio.h"
#include "history.h"
#include "keymap.h"
#include "undo.h"
#include "unicode.h"
#include "util.h"
#include "window.h"
#include <dirent.h>
#include <vterm.h>
#include <dlfcn.h>
#include <limits.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ---- the terminal and main.c boundary ------------------------------- */

struct config E;

void die(const char *s) {
	fprintf(stderr, "die: %s\n", s);
	abort();
}
void enableRawMode(void) {
}
void disableRawMode(void) {
}
void applyRawMode(void) {
}
void disableRawModeKeepScreen(void) {
}
int rawModeDivergence(char *buf, size_t n) {
	(void)buf;
	(void)n;
	return 0;
}
void installHandler(int signum, void (*handler)(int), int flags) {
	(void)signum;
	(void)handler;
	(void)flags;
}
void copyToClipboard(const uint8_t *text) {
	(void)text;
}
void openShellDrawer(void) {
}
void requestTerminalResume(void) {
}
void handlePendingSignals(void) {
}
void editorCleanup(void) {
}

/* The real deserializeUnicode (terminal.c): macro playback depends on it. */
void deserializeUnicode(void) {
	if (E.playback >= E.macro.nkeys) {
		E.unicode[0] = '?';
		E.nunicode = 1;
		return;
	}
	E.unicode[0] = E.macro.keys[E.playback++];
	E.nunicode = utf8_nBytes(E.unicode[0]);
	for (int i = 1; i < E.nunicode; i++) {
		if (E.playback >= E.macro.nkeys) {
			E.unicode[0] = '?';
			E.nunicode = 1;
			return;
		}
		E.unicode[i] = E.macro.keys[E.playback++];
	}
}

static int win_rows = 24, win_cols = 80;

void getWindowSize(int *rows, int *cols) {
	*rows = win_rows;
	*cols = win_cols;
}

/* ---- the key stream ------------------------------------------------- */

#define FK_RESIZE (-100)

struct fkey {
	int key;
	uint8_t u[4];
	int nu;
	int rows, cols; /* FK_RESIZE only */
};

static struct fkey *keys;
static int nkeys;
static int kpos;

/* ---- containment: the sandbox is the whole filesystem -----------------
 * Typed file names reach C-x C-w, C-x i, M-x cd and friends with any
 * path, including absolute ones.  Outside the sandbox that would write
 * into the real filesystem (as root, anywhere), and reads would see
 * whatever else is running (/tmp changes all the time), so runs would
 * not repeat.  These wrappers make the sandbox (HOME, resolved once at
 * startup) the only part of the filesystem that exists: writes outside
 * it fail with EACCES, reads with ENOENT, and chdir out of it fails.
 * Coverage data (*.gcda) and /dev/null are let through.
 *
 * The wrappers take the place of libc's for the editor's objects (the
 * executable's definitions win) and call the next definition, which is
 * the sanitizer's interceptor or libc. */

#ifndef RTLD_NEXT
#define RTLD_NEXT ((void *)-1l)
#endif

static char sb_root[PATH_MAX];
static size_t sb_len;
/* Only the editor is contained: on while runSequence runs, off for the
 * driver's own files (skip lists live outside the sandbox). */
static int contain_on;

static void sandboxInit(void) {
	const char *home = getenv("HOME");
	if (home && realpath(home, sb_root))
		sb_len = strlen(sb_root);
}

static void *nextFn(const char *name) {
	return dlsym(RTLD_NEXT, name);
}

/* Lexically absolute and normalised (the sandbox holds no symlinks). */
static int sandboxAllows(const char *path) {
	if (!contain_on || !sb_len || !path)
		return 1;
	size_t n = strlen(path);
	if (n > 5 && strcmp(path + n - 5, ".gcda") == 0)
		return 1;
	if (strcmp(path, "/dev/null") == 0)
		return 1;
	char abs[2 * PATH_MAX];
	if (path[0] == '/') {
		snprintf(abs, sizeof(abs), "%s", path);
	} else {
		static char *(*real_getcwd)(char *, size_t);
		if (!real_getcwd)
			real_getcwd = (char *(*)(char *, size_t))nextFn("getcwd");
		char cwd[PATH_MAX];
		if (!real_getcwd(cwd, sizeof(cwd)))
			return 0;
		snprintf(abs, sizeof(abs), "%s/%s", cwd, path);
	}
	/* Collapse "//", "." and "..". */
	char out[2 * PATH_MAX];
	size_t o = 0;
	const char *p = abs;
	while (*p) {
		while (*p == '/')
			p++;
		const char *e = p;
		while (*e && *e != '/')
			e++;
		size_t len = (size_t)(e - p);
		if (len == 0)
			break;
		if (len == 1 && p[0] == '.') {
		} else if (len == 2 && p[0] == '.' && p[1] == '.') {
			while (o > 0 && out[o - 1] != '/')
				o--;
			if (o > 0)
				o--;
		} else if (o + len + 2 < sizeof(out)) {
			out[o++] = '/';
			memcpy(out + o, p, len);
			o += len;
		}
		p = e;
	}
	if (o == 0)
		out[o++] = '/';
	out[o] = '\0';
	return strncmp(out, sb_root, sb_len) == 0 &&
	       (out[sb_len] == '\0' || out[sb_len] == '/');
}

int open(const char *path, int flags, ...) {
	static int (*real)(const char *, int, ...);
	if (!real)
		real = (int (*)(const char *, int, ...))nextFn("open");
	mode_t mode = 0;
	if (flags & O_CREAT) {
		va_list ap;
		va_start(ap, flags);
		mode = (mode_t)va_arg(ap, int);
		va_end(ap);
	}
	if (!sandboxAllows(path)) {
		errno = (flags & (O_WRONLY | O_RDWR | O_CREAT | O_TRUNC)) ? EACCES
									  : ENOENT;
		return -1;
	}
	return real(path, flags, mode);
}

FILE *fopen(const char *path, const char *mode) {
	static FILE *(*real)(const char *, const char *);
	if (!real)
		real = (FILE * (*)(const char *, const char *)) nextFn("fopen");
	if (!sandboxAllows(path)) {
		errno = strpbrk(mode, "wa+") ? EACCES : ENOENT;
		return NULL;
	}
	return real(path, mode);
}

int unlink(const char *path) {
	static int (*real)(const char *);
	if (!real)
		real = (int (*)(const char *))nextFn("unlink");
	if (!sandboxAllows(path)) {
		errno = EACCES;
		return -1;
	}
	return real(path);
}

int mkstemp(char *tmpl) {
	static int (*real)(char *);
	if (!real)
		real = (int (*)(char *))nextFn("mkstemp");
	if (!sandboxAllows(tmpl)) {
		errno = EACCES;
		return -1;
	}
	return real(tmpl);
}

DIR *opendir(const char *path) {
	static DIR *(*real)(const char *);
	if (!real)
		real = (DIR * (*)(const char *)) nextFn("opendir");
	if (!sandboxAllows(path)) {
		errno = ENOENT;
		return NULL;
	}
	return real(path);
}

int stat(const char *restrict path, struct stat *restrict st) {
	static int (*real)(const char *, struct stat *);
	if (!real)
		real = (int (*)(const char *, struct stat *))nextFn("stat");
	if (!sandboxAllows(path)) {
		errno = ENOENT;
		return -1;
	}
	return real(path, st);
}

int access(const char *path, int amode) {
	static int (*real)(const char *, int);
	if (!real)
		real = (int (*)(const char *, int))nextFn("access");
	if (!sandboxAllows(path)) {
		errno = ENOENT;
		return -1;
	}
	return real(path, amode);
}

int chdir(const char *path) {
	static int (*real)(const char *);
	if (!real)
		real = (int (*)(const char *))nextFn("chdir");
	if (!sandboxAllows(path)) {
		errno = ENOENT;
		return -1;
	}
	return real(path);
}

/* ---- time: a deterministic clock --------------------------------------
 * The editor throttles its file-modification check by CLOCK_MONOTONIC
 * (every 2 s) and bounds stat() with a 50 ms SIGALRM.  On real time, a
 * slow run under load checks files at different keys than a fast one.
 * Here time is a function of the key index: 100 ms per key, so the
 * periodic check fires at the same keys in every run of every build,
 * and the alarm never fires. */
int clock_gettime(clockid_t id, struct timespec *ts) {
	(void)id;
	long long ms = 1000000LL + (long long)kpos * 100;
	ts->tv_sec = (time_t)(ms / 1000);
	ts->tv_nsec = (long)(ms % 1000) * 1000000L;
	return 0;
}
int setitimer(__itimer_which_t which, const struct itimerval *nv,
	      struct itimerval *ov) {
	(void)which;
	(void)nv;
	if (ov)
		memset(ov, 0, sizeof(*ov));
	return 0;
}
static int exhausted_reads;
static unsigned char *skip; /* skip[i]: drop key i (minimiser) */

static uint32_t rng;
static uint32_t rnd(void) {
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}
static int pick(int n) {
	return (int)(rnd() % (uint32_t)n);
}

static const char *UNI[] = {
	"\xc3\xa9",	    /* é */
	"\xc3\x9f",	    /* ß */
	"\xe6\x97\xa5",	    /* 日 */
	"\xe6\x9c\xac",	    /* 本 */
	"\xf0\x9f\x98\x80", /* 😀 */
	"\xcc\x81",	    /* combining acute */
	"\xe2\x80\x8b",	    /* ZERO WIDTH SPACE */
	"\xe0\xb8\x81",	    /* Thai KO KAI */
	"\xe0\xb9\x80",	    /* Thai preposed vowel SARA E */
	"\xe3\x80\x82",	    /* 。 */
	"\xef\xbc\xa1",	    /* fullwidth A */
	"\xef\xbb\xbf",	    /* BOM / ZWNBSP */
	"\xc2\xa0",	    /* NBSP */
	"\xe0\xa4\xa8",	    /* Devanagari NA */
	"\xe0\xa5\x8d",	    /* virama */
};
#define NUNI ((int)(sizeof(UNI) / sizeof(UNI[0])))

static const char PRINTABLE[] =
	"aaaaeeeeiioouu     ttnnssrrll.,;:!?()[]{}<>\"'-_/\\*+=#%~`@$^&|0123456789"
	"ABCXYZbcdfghjkmpqvwxyz";

static const int CONTROLS[] = {
	'\r', '\r', '\r', '\t', '\t', KEY_CTRL('g'), KEY_CTRL('g'),
	KEY_CTRL('a'), KEY_CTRL('b'), KEY_CTRL('c'), KEY_CTRL('d'),
	KEY_CTRL('e'), KEY_CTRL('f'), KEY_CTRL('h'), KEY_CTRL('j'),
	KEY_CTRL('k'), KEY_CTRL('k'), KEY_CTRL('l'), KEY_CTRL('n'),
	KEY_CTRL('o'), KEY_CTRL('p'), KEY_CTRL('q'), KEY_CTRL('r'),
	KEY_CTRL('s'), KEY_CTRL('s'), KEY_CTRL('t'), KEY_CTRL('u'),
	KEY_CTRL('u'), KEY_CTRL('v'), KEY_CTRL('w'), KEY_CTRL('w'),
	KEY_CTRL('y'), KEY_CTRL('y'), KEY_CTRL('z'), KEY_CTRL('_'),
	KEY_CTRL('_'), 0, 0, 033, 127, 127, KEY_CTRL('x'), KEY_CTRL('x'),
	KEY_CTRL('i'), KEY_CTRL('m'), 0x1c, 0x1d, 0x1e,
};
#define NCONTROLS ((int)(sizeof(CONTROLS) / sizeof(CONTROLS[0])))

static const int SPECIALS[] = {
	KEY_ARROW_LEFT, KEY_ARROW_RIGHT, KEY_ARROW_UP, KEY_ARROW_DOWN,
	KEY_HOME,	KEY_END,	 KEY_DEL,      KEY_PAGE_UP,
	KEY_PAGE_DOWN,	KEY_BACKTAB,	 KEY_BACKSPACE,
};
#define NSPECIALS ((int)(sizeof(SPECIALS) / sizeof(SPECIALS[0])))

/* Meta characters; '|' and '!' deliberately absent. */
static const char METAS[] = "bcdfghkltuvwxyaenpz-<>.,`/%?{}:BCDFGLTUVWXY"
			    "\x06\x02\x0b\x13\x12\x7f";

static const char CX_SUB[] =
	"\x03\x13\x17\x11\x06\x12\x1f\x18\x14\x02"
	"bBhioO0128k()eEzZ\x1a"
	"uU\x15lL\x0c\x7f=x r";

static const char CXR_SUB[] = "jJ rRsStT+iIkK\x17vVyY\x1b";

static const char REGNAMES[] = "aabbc1\x07 ";

/* Files a typed file name can name: refcheck/sandbox.sh's line-ending
 * corpus plus the shared fixtures.  The file-name token below types one
 * of these into C-x C-f, C-x i or C-x C-w, so the loaders and the save
 * path see every line-ending case; random keys almost never spell a
 * file name. */
static const char *FILE_NAMES[] = {
	"lf.le",    "crlf.le",	"nonl.le",  "crlfnonl.le", "cr.le",
	"crcr.le",  "midcr.le", "tailcr.le", "mixed.le",   "empty.le",
	"nl.le",    "nul.le",	"blank.le", "a.c",	   "notes.txt",
	"new.le",   "badutf8.le", "sub",	   "sub/",	   "nodir/x.le",
};
#define NFILE_NAMES ((int)(sizeof(FILE_NAMES) / sizeof(FILE_NAMES[0])))

static void genKey(struct fkey *out, int *n, int cap) {
#define PUSH(k)                                        \
	do {                                           \
		if (*n < cap) {                        \
			memset(&out[*n], 0, sizeof(*out)); \
			out[*n].key = (k);             \
			(*n)++;                        \
		}                                      \
	} while (0)
	int r = pick(1000);
	if (r < 300) {
		PUSH((unsigned char)PRINTABLE[pick((int)sizeof(PRINTABLE) - 1)]);
	} else if (r < 350) {
		const char *u = UNI[pick(NUNI)];
		if (*n < cap) {
			memset(&out[*n], 0, sizeof(*out));
			out[*n].key = KEY_UNICODE;
			out[*n].nu = (int)strlen(u);
			memcpy(out[*n].u, u, (size_t)out[*n].nu);
			(*n)++;
		}
	} else if (r < 360) {
		/* What readKey returns for a byte that starts no valid
		 * sequence. */
		PUSH(KEY_UNICODE_ERROR);
	} else if (r < 600) {
		PUSH(CONTROLS[pick(NCONTROLS)]);
	} else if (r < 720) {
		PUSH(SPECIALS[pick(NSPECIALS)]);
	} else if (r < 880) {
		PUSH(KEY_META((unsigned char)METAS[pick((int)sizeof(METAS) - 1)]));
	} else if (r < 900) {
		PUSH(KEY_ALT_0 + pick(10));
	} else if (r < 930) {
		PUSH(KEY_CTRL('x'));
		PUSH((unsigned char)CX_SUB[pick((int)sizeof(CX_SUB) - 1)]);
	} else if (r < 950) {
		/* A file command with a real file name typed in full. */
		static const int FILE_CMDS[] = { KEY_CTRL('f'), KEY_CTRL('f'),
						 'i', 'i', 'i', KEY_CTRL('w') };
		PUSH(KEY_CTRL('x'));
		PUSH(FILE_CMDS[pick((int)(sizeof(FILE_CMDS) /
					   sizeof(FILE_CMDS[0])))]);
		const char *name = FILE_NAMES[pick(NFILE_NAMES)];
		for (const char *c = name; *c; c++)
			PUSH((unsigned char)*c);
		PUSH('\r');
	} else if (r < 990) {
		PUSH(KEY_CTRL('x'));
		PUSH('r');
		PUSH((unsigned char)CXR_SUB[pick((int)sizeof(CXR_SUB) - 1)]);
		PUSH((unsigned char)REGNAMES[pick((int)sizeof(REGNAMES) - 1)]);
	} else {
		static const int SIZES[][2] = { { 24, 80 }, { 1, 1 },  { 2, 2 },
						{ 3, 10 },  { 5, 20 }, { 10, 5 },
						{ 4, 1 },   { 60, 200 }, { 6, 7 },
						{ 2, 40 },  { 40, 3 } };
		int s = pick((int)(sizeof(SIZES) / sizeof(SIZES[0])));
		if (*n < cap) {
			memset(&out[*n], 0, sizeof(*out));
			out[*n].key = FK_RESIZE;
			out[*n].rows = SIZES[s][0];
			out[*n].cols = SIZES[s][1];
			(*n)++;
		}
	}
#undef PUSH
}

/* ---- per-sequence setup ---------------------------------------------- */

static const char *LINES[] = {
	"alpha beta gamma",
	"",
	"\tindented\twith tabs",
	"\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e\xe3\x81\xae\xe3\x83\x86\xe3\x82"
	"\xad\xe3\x82\xb9\xe3\x83\x88\xe3\x80\x82\xe4\xba\x8c",
	"\xc3\xa9 \xc3\xa0 \xc3\xbc \xc3\x9f",
	"\xf0\x9f\x98\x80 emoji \xf0\x9f\x98\x80",
	"a\xcc\x81 combining",
	"\xe0\xb8\xa0\xe0\xb8\xb2\xe0\xb8\xa9\xe0\xb8\xb2\xe2\x80\x8b\xe0\xb9"
	"\x80\xe0\xb8\x97\xe0\xb8\xa2",
	"(defun foo (x) (+ x 1))",
	"int main(void) { return foo(1); }",
	"Sentence one. Sentence two! Three?  Four.",
	"   ",
	"x",
	"\xef\xbd\x86\xef\xbd\x95\xef\xbd\x8c\xef\xbd\x8c",
	"ctl \x01 del \x7f",
	"alpha alpha alpha",
	"}",
	"{ [ ( \" ' ",
};
#define NLINES ((int)(sizeof(LINES) / sizeof(LINES[0])))

static void fillBuffer(struct buffer *buf) {
	int n = 1 + pick(40);
	for (int i = 0; i < n; i++) {
		if (pick(20) == 0) {
			/* A long line, for wrapping and horizontal scroll. */
			char line[700];
			int len = 100 + pick(500);
			for (int k = 0; k < len; k++)
				line[k] = "abc de\tf"[pick(8)];
			insertRow(buf, buf->numrows - 1, (const uint8_t *)line,
				  (size_t)len);
		} else {
			const char *l = LINES[pick(NLINES)];
			insertRow(buf, buf->numrows - 1, (const uint8_t *)l,
				  strlen(l));
		}
	}
	buf->dirty = 0;
	clearUndosAndRedos(buf);
	buf->word_wrap = pick(3) == 0;
}

static void setupEditor(void) {
	memset(&E, 0, sizeof(E));
	E.windows = xmalloc(sizeof(struct window *));
	E.windows[0] = xcalloc(1, sizeof(struct window));
	E.windows[0]->focused = 1;
	E.nwindows = 1;
	E.kill_ring_pos = -1;
	setupCommands();
	initHistory(&E.file_history);
	initHistory(&E.command_history);
	initHistory(&E.shell_history);
	initHistory(&E.search_history);
	initHistory(&E.replace_history);
	initHistory(&E.rect_history);
	initHistory(&E.kill_history);
	getWindowSize(&E.screenrows, &E.screencols);
	initFileCheck();

	E.headbuf = newBuffer();
	E.buf = E.headbuf;
	fillBuffer(E.buf);

	/* Sometimes a second, file-backed buffer. */
	if (pick(2)) {
		struct buffer *b = newBuffer();
		if (editorOpen(b, pick(2) ? "a.c" : "notes.txt") < 0) {
			destroyBuffer(b);
		} else {
			b->next = E.headbuf;
			E.headbuf = b;
			if (pick(2))
				E.buf = b;
		}
	}
	E.windows[0]->buf = E.buf;

	E.minibuf = newBuffer();
	E.minibuf->word_wrap = 0;
	E.minibuf->filename = xstrdup("*minibuffer*");
	E.minibuf->special_buffer = 1;
	E.edbuf = E.buf;
	computeDisplayNames();
}

static FILE *trace_out;
static FILE *dump_out; /* dump mode: where dumpState writes */
static int dump_at = -2; /* dump mode: key index to stop at; -1 = before any key */
static void dumpState(FILE *o);

struct fnv {
	uint64_t h;
};
static void fnvInit(struct fnv *f) {
	f->h = 1469598103934665603ULL;
}
__attribute__((no_sanitize("address", "undefined")))
static void fnvBytes(struct fnv *f, const void *p, size_t n) {
	const unsigned char *c = p;
	for (size_t i = 0; i < n; i++) {
		f->h ^= c[i];
		f->h *= 1099511628211ULL;
	}
}
/* A whole value per step (FNV-style multiply plus a shift to mix the
 * high bits down), not byte by byte: the screen hash calls this tens of
 * thousands of times per key. */
__attribute__((no_sanitize("address", "undefined")))
static void fnvInt(struct fnv *f, long v) {
	f->h ^= (uint64_t)v;
	f->h *= 1099511628211ULL;
	f->h ^= f->h >> 29;
}
static void fnvStr(struct fnv *f, const char *s) {
	if (!s) {
		fnvInt(f, -1);
		return;
	}
	size_t n = strlen(s);
	fnvInt(f, (long)n);
	fnvBytes(f, s, n);
}

/* ---- the screen: a terminal emulator behind stdout --------------------
 * emil draws with raw escape sequences written to stdout.  Comparing
 * those bytes would report every change in how a screen is drawn; what
 * matters is what is drawn.  So stdout goes into libvterm, and the
 * comparison is of its character grid: each cell's characters, width,
 * attributes and colours, plus the cursor's position and visibility.
 *
 * A snapshot is taken every time the editor asks for a key, which is
 * when it has finished drawing: at the top-level loop and inside
 * prompts, isearch, query-replace and the other modal loops.  A trace
 * line's screen field hashes every snapshot of its step, so a prompt
 * drawn wrongly while it is being typed into counts, not just the final
 * screen.  Escape sequences libvterm does not understand are hashed
 * too, so nothing is dropped silently. */

static VTerm *vt;
static VTermScreen *vts;
static int cursor_visible = 1;
static struct fnv step_screen; /* snapshots of the current step */
static int step_snaps;
static char *snap_text;        /* dump mode: this step's snapshots */
static size_t snap_len, snap_cap;
static int toplevel_read;      /* the main loop's own readKey */
/* Set by every write to the screen; cleared when a snapshot looks. */
static int vt_dirty;
static int have_last_grid;
static uint64_t last_grid;

static void snapAppend(const char *s, size_t n) {
	if (snap_len + n + 1 > snap_cap) {
		snap_cap = (snap_len + n + 1) * 2;
		snap_text = xrealloc(snap_text, snap_cap);
	}
	memcpy(snap_text + snap_len, s, n);
	snap_len += n;
	snap_text[snap_len] = '\0';
}

static int vtSetTermProp(VTermProp prop, VTermValue *val, void *user) {
	(void)user;
	if (prop == VTERM_PROP_CURSORVISIBLE)
		cursor_visible = val->boolean;
	return 1;
}

static int vtUnknownCsi(const char *leader, const long args[], int argcount,
			const char *intermed, char command, void *user) {
	(void)user;
	fnvStr(&step_screen, leader ? leader : "");
	for (int i = 0; i < argcount; i++)
		fnvInt(&step_screen, args[i]);
	fnvStr(&step_screen, intermed ? intermed : "");
	fnvInt(&step_screen, command);
	return 1;
}

static int vtUnknownOsc(int command, VTermStringFragment frag, void *user) {
	(void)user;
	fnvInt(&step_screen, command);
	fnvBytes(&step_screen, frag.str, frag.len);
	return 1;
}

static int vtUnknownControl(unsigned char control, void *user) {
	(void)user;
	fnvInt(&step_screen, 'c');
	fnvInt(&step_screen, control);
	return 1;
}

static int vtUnknownDcs(const char *command, size_t commandlen,
			VTermStringFragment frag, void *user) {
	(void)user;
	fnvBytes(&step_screen, command, commandlen);
	fnvBytes(&step_screen, frag.str, frag.len);
	return 1;
}

static int vtUnknownString(VTermStringFragment frag, void *user) {
	(void)user;
	fnvBytes(&step_screen, frag.str, frag.len);
	return 1;
}

/* Every fallback is set: libvterm calls them without checking. */
static VTermScreenCallbacks vt_cbs = { .settermprop = vtSetTermProp };
static VTermStateFallbacks vt_fallbacks = {
	.control = vtUnknownControl, .csi = vtUnknownCsi,
	.osc = vtUnknownOsc,	     .dcs = vtUnknownDcs,
	.apc = vtUnknownString,	     .pm = vtUnknownString,
	.sos = vtUnknownString,
};

/* The emulator is VT_PAD columns wider than the editor's screen.
 * Anything drawn past the right edge (a wide character on a one-column
 * screen, say) lands there instead of crashing libvterm, which cannot
 * place a two-column character on a one-column line.  The padding is
 * hashed like the rest, and the dump shows it as overflow. */
#define VT_PAD 2

static void screenInit(void) {
	if (vt)
		vterm_free(vt);
	vt = vterm_new(win_rows, win_cols + VT_PAD);
	vterm_set_utf8(vt, 1);
	vts = vterm_obtain_screen(vt);
	vterm_screen_set_callbacks(vts, &vt_cbs, NULL);
	vterm_screen_set_unrecognised_fallbacks(vts, &vt_fallbacks, NULL);
	vterm_screen_reset(vts, 1);
	cursor_visible = 1;
	vt_dirty = 1;
	have_last_grid = 0;
}

/* Debugging aid: DIFFFUZZ_TEE=FILE also records the raw output. */
static int tee_fd = -1;

/* The editor's terminal output.  Everything else passes through. */
ssize_t write(int fd, const void *buf, size_t n) {
	static ssize_t (*real)(int, const void *, size_t);
	if (!real)
		real = (ssize_t(*)(int, const void *, size_t))nextFn("write");
	if (fd == STDOUT_FILENO && vt) {
		vt_dirty = 1;
		if (tee_fd >= 0)
			real(tee_fd, buf, n);
		vterm_input_write(vt, buf, n);
		return (ssize_t)n;
	}
	return real(fd, buf, n);
}

__attribute__((no_sanitize("address", "undefined")))
static void hashColor(struct fnv *f, const VTermColor *c) {
	fnvInt(f, c->type & (VTERM_COLOR_DEFAULT_FG | VTERM_COLOR_DEFAULT_BG));
	if (VTERM_COLOR_IS_DEFAULT_FG(c) || VTERM_COLOR_IS_DEFAULT_BG(c))
		return;
	if (VTERM_COLOR_IS_INDEXED(c)) {
		fnvInt(f, 'i');
		fnvInt(f, c->indexed.idx);
	} else {
		fnvInt(f, 'r');
		fnvInt(f, c->rgb.red);
		fnvInt(f, c->rgb.green);
		fnvInt(f, c->rgb.blue);
	}
}

__attribute__((no_sanitize("address", "undefined")))
static int cellAttrs(const VTermScreenCell *c) {
	return (c->attrs.bold ? 1 : 0) | (c->attrs.underline ? 2 : 0) |
	       (c->attrs.italic ? 4 : 0) | (c->attrs.blink ? 8 : 0) |
	       (c->attrs.reverse ? 16 : 0) | (c->attrs.conceal ? 32 : 0) |
	       (c->attrs.strike ? 64 : 0);
}

__attribute__((no_sanitize("address", "undefined")))
static int cellColored(const VTermScreenCell *c) {
	return !VTERM_COLOR_IS_DEFAULT_FG(&c->fg) ||
	       !VTERM_COLOR_IS_DEFAULT_BG(&c->bg);
}

/* Fold the current grid into this step's screen hash; in dump mode
 * also keep a readable copy. */
static char *tmp_text; /* readable copy of the snapshot being taken */
static size_t tmp_len, tmp_cap;

static void tmpAppend(const char *s, size_t n) {
	if (tmp_len + n + 1 > tmp_cap) {
		tmp_cap = (tmp_len + n + 1) * 2;
		tmp_text = xrealloc(tmp_text, tmp_cap);
	}
	memcpy(tmp_text + tmp_len, s, n);
	tmp_len += n;
	tmp_text[tmp_len] = '\0';
}

__attribute__((no_sanitize("address", "undefined")))
static void snapshotScreen(void) {
	if (!vt || (!vt_dirty && have_last_grid))
		return; /* nothing drawn since the last look: same grid */
	vt_dirty = 0;
	struct fnv g;
	fnvInit(&g);
	tmp_len = 0;
	int rows, cols;
	vterm_get_size(vt, &rows, &cols);
	VTermPos cur;
	vterm_state_get_cursorpos(vterm_obtain_state(vt), &cur);
	fnvInt(&g, rows);
	fnvInt(&g, cols);
	fnvInt(&g, cursor_visible);
	if (cursor_visible) {
		fnvInt(&g, cur.row);
		fnvInt(&g, cur.col);
	}
	char hdr[96];
	if (dump_out) {
		snprintf(hdr, sizeof(hdr), "  snapshot %d, %dx%d, cursor %s %d,%d\n",
			 step_snaps + 1, rows, cols - VT_PAD,
			 cursor_visible ? "at" : "hidden", cur.row + 1,
			 cur.col + 1);
		tmpAppend(hdr, strlen(hdr));
	}
	for (int r = 0; r < rows; r++) {
		char text[4 * 4 * 256 + 8], marks[256 + 8];
		size_t tl = 0, edge = 0;
		int ml = 0, any_mark = 0;
		for (int c = 0; c < cols; c++) {
			if (c == cols - VT_PAD)
				edge = tl; /* text before this is on screen */
			VTermScreenCell cell;
			VTermPos pos = { .row = r, .col = c };
			vterm_screen_get_cell(vts, pos, &cell);
			fnvInt(&g, cell.width);
			for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++)
				fnvInt(&g, (long)cell.chars[i]);
			fnvInt(&g, -1);
			fnvInt(&g, cellAttrs(&cell));
			hashColor(&g, &cell.fg);
			hashColor(&g, &cell.bg);
			if (!dump_out || c >= 256)
				continue;
			/* Readable copy: the text, and a marks line under it
			 * for attributes (R reverse, B bold, U underline, C
			 * colour, * several). */
			int a = cellAttrs(&cell), col = cellColored(&cell);
			char m = ' ';
			if (a == 16 && !col) m = 'R';
			else if (a == 1 && !col) m = 'B';
			else if (a == 2 && !col) m = 'U';
			else if (a == 0 && col) m = 'C';
			else if (a || col) m = '*';
			if (m != ' ')
				any_mark = 1;
			if (cell.width == 0)
				continue; /* second half of a wide character */
			marks[ml++] = m;
			if (cell.width == 2)
				marks[ml++] = m;
			if (!cell.chars[0]) {
				text[tl++] = ' ';
				continue;
			}
			for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) {
				uint8_t u[4];
				int n = utf8Encode(cell.chars[i], u);
				if (cell.chars[i] < 32 || cell.chars[i] == 127) {
					tl += (size_t)snprintf(text + tl, 8, "\\x%02x",
							       (unsigned)cell.chars[i]);
					continue;
				}
				memcpy(text + tl, u, (size_t)n);
				tl += (size_t)n;
			}
		}
		if (!dump_out)
			continue;
		size_t over = tl;
		while (over > edge && text[over - 1] == ' ')
			over--;
		size_t on = edge;
		while (on > 0 && text[on - 1] == ' ')
			on--;
		char line[sizeof(text) + 64];
		int n = snprintf(line, sizeof(line), "    %2d |%.*s|", r + 1,
				 (int)on, text);
		if (over > edge)
			n += snprintf(line + n, sizeof(line) - (size_t)n,
				      "  past the edge: \"%.*s\"",
				      (int)(over - edge), text + edge);
		line[n++] = '\n';
		tmpAppend(line, (size_t)n);
		if (any_mark) {
			while (ml > 0 && marks[ml - 1] == ' ')
				ml--;
			n = snprintf(line, sizeof(line), "       %.*s\n", ml, marks);
			tmpAppend(line, (size_t)n);
		}
	}
	/* A redraw that leaves the grid as it was is not a new screen, so
	 * adding or removing a redundant redraw is not a divergence. */
	if (have_last_grid && g.h == last_grid)
		return;
	have_last_grid = 1;
	last_grid = g.h;
	fnvInt(&step_screen, (long)g.h);
	if (dump_out)
		snapAppend(tmp_text ? tmp_text : "", tmp_len);
	step_snaps++;
}

static void stepScreenReset(void) {
	fnvInit(&step_screen);
	step_snaps = 0;
	snap_len = 0;
	if (snap_text)
		snap_text[0] = '\0';
}

/* ---- readKey --------------------------------------------------------- */

static int nextRaw(struct fkey *fk) {
	while (kpos < nkeys) {
		int i = kpos++;
		if (skip && skip[i])
			continue;
		*fk = keys[i];
		return 1;
	}
	return 0;
}

int readKey(void) {
	/* The editor has drawn and is waiting: the screen is settled.  The
	 * main loop takes its own snapshot, as the end of the previous
	 * step. */
	if (!toplevel_read && !E.playback)
		snapshotScreen();
	toplevel_read = 0;
	if (E.playback) {
		if (E.playback >= E.macro.nkeys)
			return -1;
		int ret = E.macro.keys[E.playback++];
		if (ret == KEY_UNICODE)
			deserializeUnicode();
		return ret;
	}
	for (;;) {
		struct fkey fk;
		if (!nextRaw(&fk)) {
			/* Out of keys: cancel whatever is still asking. */
			if (++exhausted_reads > 5000) {
				fprintf(stderr, "HANG: modal loop still reading "
						"after 5000 C-g\n");
				_exit(3);
			}
			return KEY_CTRL('g');
		}
		if (fk.key == FK_RESIZE) {
			win_rows = fk.rows;
			win_cols = fk.cols;
			if (vt) {
				vterm_set_size(vt, win_rows, win_cols + VT_PAD);
				vt_dirty = 1;
			}
			resizeScreen();
			continue;
		}
		if (fk.key == KEY_UNICODE) {
			memcpy(E.unicode, fk.u, 4);
			E.nunicode = fk.nu;
		}
		return fk.key;
	}
}

/* ---- leads: invariants that are not themselves memory errors --------- */

static int lead_reported;
static const char *last_lead = "";

static int bufferInList(struct buffer *b) {
	for (struct buffer *x = E.headbuf; x; x = x->next)
		if (x == b)
			return 1;
	return 0;
}

static void lead(const char *what) {
	if (lead_reported)
		return;
	lead_reported = 1;
	last_lead = what;
	fprintf(stderr, "LEAD at key %d: %s\n", kpos, what);
}

static void checkLeads(void) {
	const char *breach = focusInvariantBreach();
	if (breach)
		lead(breach);
	for (int i = 0; i < E.nwindows; i++)
		if (!bufferInList(E.windows[i]->buf))
			lead("a window shows a buffer not in the list");
	if (!bufferInList(E.buf))
		lead("E.buf not in the buffer list");
	struct buffer *b = E.buf;
	if (b->numrows < 1) {
		lead("numrows < 1");
		return;
	}
	if (b->cy < 0 || b->cy >= b->numrows)
		lead("cy out of range");
	else if (b->cx < 0 || b->cx > b->row[b->cy].size)
		lead("cx out of range");
	for (int i = 0; i < b->numrows; i++) {
		if (!utf8_validate(b->row[i].chars, b->row[i].size)) {
			lead("row is not valid UTF-8");
			break;
		}
	}
}


/* ---- state digests (trace mode) --------------------------------------- */

static int bufferIndex(struct buffer *b) {
	int i = 0;
	for (struct buffer *x = E.headbuf; x; x = x->next, i++)
		if (x == b)
			return i;
	return b == E.minibuf ? -2 : -1;
}

static void hashText(struct fnv *f, const struct text *t) {
	fnvStr(f, (const char *)t->str);
	fnvInt(f, t->is_rectangle);
	if (t->is_rectangle) {
		fnvInt(f, t->rect_width);
		fnvInt(f, t->rect_height);
	}
}

static int cmpStr(const void *a, const void *b) {
	return strcmp(*(char *const *)a, *(char *const *)b);
}

/* Names and contents of every file under dir, in sorted order. */
static void hashDir(struct fnv *f, const char *dir) {
	DIR *d = opendir(dir);
	if (!d)
		return;
	char **names = NULL;
	int n = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			names = xrealloc(names, sizeof(*names) * (size_t)cap);
		}
		names[n++] = xstrdup(e->d_name);
	}
	closedir(d);
	if (n > 1)
		qsort(names, (size_t)n, sizeof(*names), cmpStr);
	for (int i = 0; i < n; i++) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		struct stat st;
		fnvStr(f, names[i]);
		if (lstat(path, &st) != 0) {
			fnvInt(f, -1);
		} else if (S_ISDIR(st.st_mode)) {
			fnvInt(f, 'd');
			hashDir(f, path);
		} else if (S_ISREG(st.st_mode)) {
			fnvInt(f, 'f');
			fnvInt(f, (long)(st.st_mode & 0777));
			FILE *fp = fopen(path, "rb");
			if (fp) {
				char buf[4096];
				size_t r;
				while ((r = fread(buf, 1, sizeof(buf), fp)) > 0)
					fnvBytes(f, buf, r);
				fclose(fp);
			}
		} else {
			fnvInt(f, 'o');
		}
		free(names[i]);
	}
	free(names);
}

static void digestState(int index) {
	struct fnv buf, cur, mark, status, kill, reg, win, files, lead_h;
	struct fnv screen_h = step_screen;
	fnvInit(&buf);
	fnvInit(&cur);
	fnvInit(&mark);
	fnvInit(&status);
	fnvInit(&kill);
	fnvInit(&reg);
	fnvInit(&win);
	fnvInit(&files);
	fnvInit(&lead_h);

	/* Every buffer in list order: name, text, flags. */
	for (struct buffer *b = E.headbuf; b; b = b->next) {
		fnvStr(&buf, b->filename);
		fnvInt(&buf, b->numrows);
		for (int i = 0; i < b->numrows; i++) {
			fnvInt(&buf, b->row[i].size);
			fnvBytes(&buf, b->row[i].chars, (size_t)b->row[i].size);
		}
		fnvInt(&buf, b->dirty != 0);
		fnvInt(&buf, b->read_only);
		fnvInt(&buf, b->word_wrap);
		fnvInt(&buf, b->special_buffer);
	}

	/* Point: which buffer is current, and where in it. */
	fnvInt(&cur, bufferIndex(E.buf));
	fnvInt(&cur, E.buf->cx);
	fnvInt(&cur, E.buf->cy);

	fnvInt(&mark, E.buf->mark_active);
	fnvInt(&mark, E.buf->markx);
	fnvInt(&mark, E.buf->marky);

	/* What the user would see in the echo area. */
	fnvInt(&status, E.statusmsg_show != 0);
	if (E.statusmsg_show)
		fnvStr(&status, E.statusmsg);

	hashText(&kill, &E.kill);
	for (struct historyEntry *h = E.kill_history.head; h; h = h->next)
		fnvStr(&kill, h->str);

	for (int i = 0; i < 127; i++) {
		fnvInt(&reg, E.registers[i].rtype);
		if (E.registers[i].rtype == REGISTER_NULL)
			continue;
		if (E.registers[i].rtype == REGISTER_POINT) {
			fnvInt(&reg, E.registers[i].data.point.cx);
			fnvInt(&reg, E.registers[i].data.point.cy);
			fnvInt(&reg,
			       bufferIndex(E.registers[i].data.point.buf));
		} else {
			hashText(&reg, &E.registers[i].data.text);
		}
	}

	fnvInt(&win, E.nwindows);
	for (int i = 0; i < E.nwindows; i++) {
		fnvInt(&win, E.windows[i]->focused);
		fnvInt(&win, bufferIndex(E.windows[i]->buf));
	}

	hashDir(&files, ".");

	fnvStr(&lead_h, last_lead);

	fprintf(trace_out,
		"%d buf=%016llx cur=%016llx mark=%016llx status=%016llx "
		"kill=%016llx reg=%016llx win=%016llx files=%016llx "
		"lead=%016llx screen=%016llx\n",
		index, (unsigned long long)buf.h, (unsigned long long)cur.h,
		(unsigned long long)mark.h, (unsigned long long)status.h,
		(unsigned long long)kill.h, (unsigned long long)reg.h,
		(unsigned long long)win.h, (unsigned long long)files.h,
		(unsigned long long)lead_h.h, (unsigned long long)screen_h.h);
	fflush(trace_out);
}

/* Readable form of the same state, for witnesses in the report.  One
 * section per digest field, so the report can show only the sections
 * that differ. */
static void dumpEsc(FILE *o, const uint8_t *s, int n) {
	for (int i = 0; i < n; i++) {
		unsigned c = s[i];
		if (c == '\\')
			fputs("\\\\", o);
		else if (c == '\t')
			fputs("\\t", o);
		else if (c == '\r')
			fputs("\\r", o);
		else if (c < 32 || c == 127)
			fprintf(o, "\\x%02x", c);
		else
			fputc((int)c, o);
	}
}

static void dumpDir(FILE *o, const char *dir) {
	DIR *d = opendir(dir);
	if (!d)
		return;
	char **names = NULL;
	int n = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 16;
			names = xrealloc(names, sizeof(*names) * (size_t)cap);
		}
		names[n++] = xstrdup(e->d_name);
	}
	closedir(d);
	if (n > 1)
		qsort(names, (size_t)n, sizeof(*names), cmpStr);
	for (int i = 0; i < n; i++) {
		char path[4096];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		struct stat st;
		if (lstat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
			dumpDir(o, path);
		} else if (lstat(path, &st) == 0 && S_ISREG(st.st_mode)) {
			fprintf(o, "  %s (%lld bytes): \"", path + 2,
				(long long)st.st_size);
			FILE *fp = fopen(path, "rb");
			if (fp) {
				uint8_t buf[200];
				size_t r = fread(buf, 1, sizeof(buf), fp);
				dumpEsc(o, buf, (int)r);
				if (st.st_size > (off_t)sizeof(buf))
					fputs("...", o);
				fclose(fp);
			}
			fputs("\"\n", o);
		}
		free(names[i]);
	}
	free(names);
}

static void dumpState(FILE *o) {
	fprintf(o, "[buf]\n");
	int bi = 0;
	for (struct buffer *b = E.headbuf; b; b = b->next, bi++) {
		fprintf(o, "  buffer %d %s%s%s%s\n", bi,
			b->filename ? b->filename : "(no name)",
			b->dirty ? " modified" : "",
			b->read_only ? " read-only" : "",
			b->special_buffer ? " special" : "");
		int shown = b->numrows < 12 ? b->numrows : 12;
		for (int i = 0; i < shown; i++) {
			fprintf(o, "    %3d \"", i + 1);
			dumpEsc(o, b->row[i].chars, b->row[i].size);
			fputs("\"\n", o);
		}
		if (shown < b->numrows)
			fprintf(o, "    ... %d more rows\n", b->numrows - shown);
	}
	fprintf(o, "[cur]\n  buffer %d, line %d, byte %d\n",
		bufferIndex(E.buf), E.buf->cy + 1, E.buf->cx);
	fprintf(o, "[mark]\n  %s at line %d, byte %d\n",
		E.buf->mark_active ? "active" : "inactive", E.buf->marky + 1,
		E.buf->markx);
	fprintf(o, "[status]\n  %s\n",
		E.statusmsg_show ? E.statusmsg : "(none shown)");
	fprintf(o, "[kill]\n  \"");
	if (E.kill.str)
		dumpEsc(o, E.kill.str, (int)strlen((char *)E.kill.str));
	fprintf(o, "\"\n");
	fprintf(o, "[reg]\n");
	for (int i = 0; i < 127; i++) {
		if (E.registers[i].rtype == REGISTER_POINT)
			fprintf(o, "  %d: point line %d byte %d\n", i,
				E.registers[i].data.point.cy + 1,
				E.registers[i].data.point.cx);
		else if (E.registers[i].rtype != REGISTER_NULL &&
			 E.registers[i].data.text.str) {
			fprintf(o, "  %d: \"", i);
			uint8_t *t = E.registers[i].data.text.str;
			dumpEsc(o, t, (int)strlen((char *)t));
			fprintf(o, "\"\n");
		}
	}
	fprintf(o, "[win]\n  %d window(s)", E.nwindows);
	for (int i = 0; i < E.nwindows; i++)
		fprintf(o, "%s buffer %d", i ? "," : ":",
			bufferIndex(E.windows[i]->buf));
	fprintf(o, "\n[files]\n");
	dumpDir(o, ".");
	fprintf(o, "[lead]\n  %s\n", last_lead[0] ? last_lead : "(none)");
	fprintf(o, "[screen]\n%s", snap_text ? snap_text : "");
}

/* ---- one sequence ---------------------------------------------------- */

static void generate(uint32_t seed) {
	rng = seed * 2654435761u + 1;
	if (!rng)
		rng = 1;
	static const int SIZES[][2] = { { 24, 80 }, { 24, 80 }, { 1, 1 },
					{ 2, 2 },   { 3, 10 },	{ 5, 20 },
					{ 10, 5 },  { 4, 1 },	{ 60, 200 },
					{ 7, 30 } };
	int s = pick((int)(sizeof(SIZES) / sizeof(SIZES[0])));
	win_rows = SIZES[s][0];
	win_cols = SIZES[s][1];
	int cap = 200 + pick(1500);
	free(keys);
	keys = xmalloc(sizeof(*keys) * (size_t)cap);
	nkeys = 0;
	while (nkeys < cap)
		genKey(keys, &nkeys, cap);
	/* The buffer contents come from the same stream, after the keys,
	 * so dropping keys in the minimiser does not change them. */
}

static void runSequence(uint32_t seed) {
	generate(seed);
	contain_on = 1;
	setupEditor();
	kpos = 0;
	exhausted_reads = 0;
	if (dump_out && dump_at == -1) {
		dumpState(dump_out);
		fclose(dump_out);
		exit(0); /* not _exit: coverage data is written at exit */
	}
	screenInit();
	stepScreenReset();
	int stepped = 0;
	for (;;) {
		refreshScreen();
		/* The previous step ends here: its key has been processed
		 * and the result drawn. */
		if (stepped) {
			snapshotScreen();
			if (trace_out)
				digestState(kpos);
			if (dump_out && kpos >= dump_at) {
				dumpState(dump_out);
				fclose(dump_out);
				exit(0); /* not _exit: coverage data is written at exit */
			}
		}
		stepScreenReset();
		if (kpos >= nkeys)
			break;
		toplevel_read = 1;
		int key = readKey();
		stepped = 1;
		if (key == -1)
			continue;
		recordKey(key);
		if (key >= ' ' && key < KEY_ARROW_LEFT)
			E.self_insert_key = key;
		if (key != 033)
			E.statusmsg_show = 0;
		int cmd = resolveBinding(key);
		if (cmd != CMD_NONE)
			processKeypress(cmd);
		/* Keyed to the key's index, not the generator, so the
		 * minimiser dropping keys does not move the others' runs. */
		if (kpos % 7 == 0)
			undoCloseRun(E.buf);
		checkLeads();
	}
	contain_on = 0;
}

/* ---- key names, for reports ------------------------------------------ */

static void keyName(const struct fkey *k, char *out, size_t n) {
	int c = k->key;
	if (c == FK_RESIZE) {
		snprintf(out, n, "<resize %dx%d>", k->rows, k->cols);
	} else if (c == KEY_UNICODE) {
		snprintf(out, n, "U+%04X", (unsigned)utf8Decode(k->u, 0));
	} else if (c == KEY_UNICODE_ERROR) {
		snprintf(out, n, "<utf8-error>");
	} else if (c >= KEY_ALT_0 && c <= KEY_ALT_9) {
		snprintf(out, n, "M-%d", c - KEY_ALT_0);
	} else if (IS_META_KEY(c)) {
		int m = META_CHAR(c);
		if (m < 32)
			snprintf(out, n, "C-M-%c", m + '`');
		else if (m == 127)
			snprintf(out, n, "M-DEL");
		else
			snprintf(out, n, "M-%c", m);
	} else if (c >= KEY_ARROW_LEFT) {
		static const char *names[] = { "<left>", "<right>", "<up>",
					       "<down>", "<home>",  "<del>",
					       "<end>",	 "<pgup>",  "<pgdn>",
					       "<backtab>" };
		int i = c - KEY_ARROW_LEFT;
		snprintf(out, n, "%s",
			 i >= 0 && i < 10 ? names[i] : "<key?>");
	} else if (c == '\r') {
		snprintf(out, n, "RET");
	} else if (c == '\t') {
		snprintf(out, n, "TAB");
	} else if (c == 033) {
		snprintf(out, n, "ESC");
	} else if (c == 127) {
		snprintf(out, n, "DEL");
	} else if (c == ' ') {
		snprintf(out, n, "SPC");
	} else if (c == 0) {
		snprintf(out, n, "C-@");
	} else if (c < 32) {
		snprintf(out, n, "C-%c", c + '`');
	} else if (c >= 0x80) {
		snprintf(out, n, "<byte 0x%02x>", c);
	} else {
		snprintf(out, n, "%c", c);
	}
}

static void showKeys(void) {
	char name[64];
	int col = 0;
	for (int i = 0; i < nkeys; i++) {
		if (skip && skip[i])
			continue;
		keyName(&keys[i], name, sizeof(name));
		int w = (int)strlen(name) + 1;
		if (col + w > 76) {
			printf("\n");
			col = 0;
		}
		printf("%s ", name);
		col += w;
	}
	printf("\n");
}

/* ---- forking driver --------------------------------------------------- */

/* Run one sequence in a child.  Returns the child's status class:
 * 0 clean, 1 sanitizer/crash, 2 hang, 3 lead only.  The child's stderr
 * goes to errpath. */
static int runChild(uint32_t seed, const char *errpath, int timeout_s) {
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		int dn = open("/dev/null", O_WRONLY);
		dup2(dn, STDOUT_FILENO);
		int ef = open(errpath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(ef, STDERR_FILENO);
		signal(SIGTSTP, SIG_IGN);
		runSequence(seed);
		_exit(lead_reported ? 4 : 0);
	}
	int status;
	for (int t = 0; t < timeout_s * 20; t++) {
		pid_t w = waitpid(pid, &status, WNOHANG);
		if (w == pid) {
			if (WIFEXITED(status)) {
				int code = WEXITSTATUS(status);
				if (code == 0)
					return 0;
				if (code == 4)
					return 3;
				if (code == 3)
					return 2;
				return 1;
			}
			return 1;
		}
		struct timespec ts = { 0, 50000000 };
		nanosleep(&ts, NULL);
	}
	kill(pid, SIGKILL);
	waitpid(pid, &status, 0);
	return 2;
}

/* First "#N 0x... in FUNC" frame of the report that is not the
 * sanitizer runtime or libc, as a crash signature. */
static void signature(const char *errpath, char *sig, size_t n) {
	FILE *f = fopen(errpath, "r");
	sig[0] = '\0';
	if (!f)
		return;
	char line[1024];
	char kind[128] = "";
	while (fgets(line, sizeof(line), f)) {
		if (!kind[0]) {
			char *p = strstr(line, "ERROR: AddressSanitizer: ");
			if (p) {
				sscanf(p + 25, "%127s", kind);
				continue;
			}
			p = strstr(line, "runtime error: ");
			if (p) {
				snprintf(kind, sizeof(kind), "ubsan");
				char *c = strstr(line, ".c:");
				if (c) {
					char *s = c;
					while (s > line && s[-1] != '/' &&
					       s[-1] != ' ')
						s--;
					char *e = strchr(c + 3, ':');
					if (e)
						*e = '\0';
					snprintf(sig, n, "ubsan %s", s);
					fclose(f);
					return;
				}
			}
			if (strstr(line, "HANG:")) {
				snprintf(sig, n, "hang");
				fclose(f);
				return;
			}
		}
		char *in = strstr(line, " in ");
		if (kind[0] && line[0] == ' ' && strstr(line, "#") && in) {
			char fn[256];
			if (sscanf(in + 4, "%255s", fn) == 1 &&
			    strncmp(fn, "__", 2) != 0 &&
			    strncmp(fn, "mem", 3) != 0 &&
			    strncmp(fn, "str", 3) != 0) {
				snprintf(sig, n, "%s %s", kind, fn);
				fclose(f);
				return;
			}
		}
	}
	if (kind[0])
		snprintf(sig, n, "%s", kind);
	fclose(f);
}

static void loadSkip(const char *path) {
	skip = xcalloc((size_t)nkeys + 1, 1);
	if (!path)
		return;
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	int i;
	while (fscanf(f, "%d", &i) == 1)
		if (i >= 0 && i < nkeys)
			skip[i] = 1;
	fclose(f);
}

/* Greedy chunked reduction: try dropping runs of keys, halving the run
 * length, keeping any drop that preserves the failure signature. */
static void minimise(uint32_t seed) {
	char errpath[] = "/tmp/fuzz_keys_min.err";
	char want[512], got[512];
	generate(seed);
	int total = nkeys;
	skip = xcalloc((size_t)total + 1, 1);
	if (runChild(seed, errpath, 30) == 0) {
		printf("seed %u does not fail\n", seed);
		return;
	}
	signature(errpath, want, sizeof(want));
	printf("signature: %s\n", want);
	for (int chunk = total / 2; chunk >= 1; chunk /= 2) {
		for (int start = 0; start < total; start += chunk) {
			unsigned char saved[4096];
			int any = 0;
			for (int i = start; i < start + chunk && i < total; i++) {
				saved[i - start] = skip[i];
				if (!skip[i])
					any = 1;
				skip[i] = 1;
			}
			if (!any)
				continue;
			int rc = runChild(seed, errpath, 30);
			signature(errpath, got, sizeof(got));
			if (rc != 0 && rc != 3 && strcmp(got, want) == 0)
				continue; /* keep the drop */
			for (int i = start; i < start + chunk && i < total; i++)
				skip[i] = saved[i - start];
		}
	}
	FILE *f = fopen("min.skip", "w");
	int kept = 0;
	for (int i = 0; i < total; i++) {
		if (skip[i])
			fprintf(f, "%d\n", i);
		else
			kept++;
	}
	fclose(f);
	printf("kept %d of %d keys (skip list in min.skip):\n", kept, total);
	generate(seed);
	showKeys();
}

int main(int argc, char **argv) {
	(void)selectUtf8Locale();
	sandboxInit();
	if (getenv("DIFFFUZZ_TEE"))
		tee_fd = open(getenv("DIFFFUZZ_TEE"),
			      O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0644);
	if (argc >= 4 && strcmp(argv[1], "run") == 0) {
		uint32_t first = (uint32_t)strtoul(argv[2], NULL, 10);
		uint32_t count = (uint32_t)strtoul(argv[3], NULL, 10);
		char errpath[64];
		snprintf(errpath, sizeof(errpath), "/tmp/fuzz_keys.%d.err",
			 (int)getpid());
		for (uint32_t s = first; s < first + count; s++) {
			int rc = runChild(s, errpath, 20);
			if (rc == 0)
				continue;
			char sig[512];
			signature(errpath, sig, sizeof(sig));
			if (rc == 3) {
				FILE *f = fopen(errpath, "r");
				char line[256] = "";
				if (f) {
					if (!fgets(line, sizeof(line), f))
						line[0] = '\0';
					fclose(f);
				}
				printf("seed %u LEAD %s", s, line);
			} else {
				printf("seed %u %s %s\n", s,
				       rc == 2 ? "HANG" : "FAIL", sig);
				char keep[128];
				snprintf(keep, sizeof(keep), "fail.%u.err", s);
				FILE *in = fopen(errpath, "r");
				FILE *out = fopen(keep, "w");
				if (in && out) {
					char buf[4096];
					size_t r;
					while ((r = fread(buf, 1, sizeof(buf),
							  in)) > 0)
						fwrite(buf, 1, r, out);
				}
				if (in)
					fclose(in);
				if (out)
					fclose(out);
			}
			fflush(stdout);
		}
		unlink(errpath);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "trace") == 0) {
		uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 10);
		generate(seed);
		loadSkip(argc >= 4 && strcmp(argv[3], "-") ? argv[3] : NULL);
		trace_out = fdopen(3, "w");
		if (!trace_out) {
			fprintf(stderr, "trace: fd 3 is not open\n");
			return 2;
		}
		/* How many keys the skip file actually removed, so the
		 * harness can tell a skip list that did not load from one
		 * that made no difference. */
		int nskip = 0;
		for (int i = 0; i < nkeys; i++)
			nskip += skip && skip[i];
		fprintf(trace_out, "skip %d\n", nskip);
		int dn = open("/dev/null", O_WRONLY);
		dup2(dn, STDOUT_FILENO);
		signal(SIGTSTP, SIG_IGN);
		runSequence(seed);
		fprintf(trace_out, "end\n");
		fclose(trace_out);
		return 0;
	}
	if (argc >= 5 && strcmp(argv[1], "dump") == 0) {
		/* dump SEED SKIPFILE AT: readable state after the key that
		 * brings the trace index to AT (-1: before any key).  Use
		 * "-" for no skip file. */
		uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 10);
		generate(seed);
		loadSkip(strcmp(argv[3], "-") ? argv[3] : NULL);
		dump_at = atoi(argv[4]);
		dump_out = fdopen(3, "w");
		if (!dump_out) {
			fprintf(stderr, "dump: fd 3 is not open\n");
			return 2;
		}
		int dn = open("/dev/null", O_WRONLY);
		dup2(dn, STDOUT_FILENO);
		signal(SIGTSTP, SIG_IGN);
		runSequence(seed);
		dumpState(dump_out);
		fclose(dump_out);
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "keys") == 0) {
		/* "INDEX NAME" per key, for witnesses; plus the screen size
		 * and the initial buffer, which come from the same seed. */
		uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 10);
		generate(seed);
		printf("screen %dx%d\n", win_rows, win_cols);
		char name[64];
		for (int i = 0; i < nkeys; i++) {
			keyName(&keys[i], name, sizeof(name));
			printf("%d %s\n", i, name);
		}
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "replay") == 0) {
		uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 10);
		generate(seed);
		loadSkip(argc >= 4 ? argv[3] : NULL);
		int dn = open("/dev/null", O_WRONLY);
		int saved = dup(STDOUT_FILENO);
		dup2(dn, STDOUT_FILENO);
		signal(SIGTSTP, SIG_IGN);
		runSequence(seed);
		dup2(saved, STDOUT_FILENO);
		printf("replay of seed %u finished%s\n", seed,
		       lead_reported ? " (with a lead)" : "");
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "show") == 0) {
		uint32_t seed = (uint32_t)strtoul(argv[2], NULL, 10);
		generate(seed);
		loadSkip(argc >= 4 ? argv[3] : NULL);
		printf("seed %u: %dx%d screen\n", seed, win_rows, win_cols);
		showKeys();
		return 0;
	}
	if (argc >= 3 && strcmp(argv[1], "min") == 0) {
		minimise((uint32_t)strtoul(argv[2], NULL, 10));
		return 0;
	}
	fprintf(stderr, "usage: fuzz_keys run FIRST COUNT | replay SEED "
			"[SKIP] | show SEED [SKIP] | min SEED\n");
	return 2;
}
