/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* fuzz_keys.c: whole-editor keystroke fuzzer, for memory-safety hunting.
 *
 * A pre-1.0 audit tool, not a suite: it is not listed in SUITES and
 * run_tests.sh never builds it.  Build and run it with
 * tests/fuzz_keys.sh, which compiles the editor's objects under
 * AddressSanitizer and UndefinedBehaviorSanitizer.
 *
 * Where test_fuzz.c drives a fixed list of non-prompting commands
 * through processKeypress, this drives *keys* through the same path the
 * main loop uses (recordKey, resolveBinding, processKeypress, and a
 * refreshScreen before every key), with readKey() replaced by a
 * seeded generator.  Every modal loop -- prompts, completion, isearch,
 * query-replace, registers, zap, confirmations, the palette -- reads
 * from that generator too, so all of them are in reach.  The screen is
 * rendered at a size chosen per sequence, including degenerate ones,
 * and is occasionally resized mid-sequence.
 *
 * Each sequence runs in a forked child, so a sanitizer abort, a crash
 * or a hang is attributed to exactly one seed and the next seed starts
 * from a clean process.
 *
 * Not in the key alphabet: M-| and M-! (they run arbitrary shell
 * commands built from random bytes).  C-x C-c is in it, and simply
 * ends the sequence through exit().
 *
 * Usage:
 *   fuzz_keys run FIRST_SEED COUNT       fuzz; failures logged to stdout
 *   fuzz_keys replay SEED [SKIPFILE]     one sequence in-process
 *   fuzz_keys show SEED [SKIPFILE]       print the key sequence
 *   fuzz_keys min SEED                   reduce a failing sequence
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
#include <errno.h>
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
const int page_overlap = 2;

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
	'\r', '\r', '\r', '\t', '\t', CTRL('g'), CTRL('g'), CTRL('a'),
	CTRL('b'), CTRL('c'), CTRL('d'), CTRL('e'), CTRL('f'), CTRL('h'),
	CTRL('j'), CTRL('k'), CTRL('k'), CTRL('l'), CTRL('n'), CTRL('o'),
	CTRL('p'), CTRL('q'), CTRL('r'), CTRL('s'), CTRL('s'), CTRL('t'),
	CTRL('u'), CTRL('u'), CTRL('v'), CTRL('w'), CTRL('w'), CTRL('y'),
	CTRL('y'), CTRL('z'), CTRL('_'), CTRL('_'), 0, 0, 033, 127, 127,
	CTRL('x'), CTRL('x'), CTRL('i'), CTRL('m'), 0x1c, 0x1d, 0x1e,
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
	} else if (r < 356) {
		/* What readKey returns for a byte that starts no valid
		 * sequence: continuation bytes and the never-valid leads. */
		static const int RAW[] = { 0x80, 0x9b, 0xbf, 0xc0,
					   0xc1, 0xf5, 0xf8, 0xff };
		PUSH(RAW[pick(8)]);
	} else if (r < 360) {
		PUSH(KEY_UNICODE_ERROR);
	} else if (r < 600) {
		PUSH(CONTROLS[pick(NCONTROLS)]);
	} else if (r < 720) {
		PUSH(SPECIALS[pick(NSPECIALS)]);
	} else if (r < 880) {
		PUSH(KEY_META((unsigned char)METAS[pick((int)sizeof(METAS) - 1)]));
	} else if (r < 900) {
		PUSH(KEY_ALT_0 + pick(10));
	} else if (r < 950) {
		PUSH(CTRL('x'));
		PUSH((unsigned char)CX_SUB[pick((int)sizeof(CX_SUB) - 1)]);
	} else if (r < 990) {
		PUSH(CTRL('x'));
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
			return CTRL('g');
		}
		if (fk.key == FK_RESIZE) {
			win_rows = fk.rows;
			win_cols = fk.cols;
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
	setupEditor();
	kpos = 0;
	exhausted_reads = 0;
	for (;;) {
		refreshScreen();
		if (kpos >= nkeys)
			break;
		int key = readKey();
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
