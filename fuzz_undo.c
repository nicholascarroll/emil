/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* Invariant fuzzer for emil's buffer/undo layer.  Not part of the build.
 *
 * Drives random sequences of real commands through processKeypress and
 * checks, after every operation:
 *
 *   - every row is valid UTF-8
 *   - the cursor is in bounds and on a character boundary
 *   - the buffer ends in a newline unless it is empty
 *
 * and, after the whole sequence:
 *
 *   - undoing everything restores the original buffer content
 *
 * A failing sequence is then delta-debugged: operations are dropped one
 * at a time for as long as the failure persists, which usually reduces a
 * sixty-operation sequence to two or three.
 *
 * Build:
 *   cc -std=c99 -D_DEFAULT_SOURCE -D_BSD_SOURCE -g -O0 -I. -Itests \
 *      -o /tmp/fuzz fuzz_undo.c $(ls *.o | grep -vE '^(main|terminal)\.o$') \
 *      /tmp/stubs.o
 *   /tmp/fuzz [iterations] [seed]
 */
#include "test_harness.h"
#include "buffer.h"
#include "keymap.h"
#include "fileio.h"
#include "undo.h"
#include "unicode.h"
#include "util.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>

/* ---- plumbing ---------------------------------------------------- */

static int saved_stdout = -1;

static void muteStdout(void) {
	fflush(stdout);
	saved_stdout = dup(STDOUT_FILENO);
	int devnull = open("/dev/null", O_WRONLY);
	if (devnull >= 0) {
		dup2(devnull, STDOUT_FILENO);
		close(devnull);
	}
}

static void unmuteStdout(void) {
	fflush(stdout);
	if (saved_stdout >= 0) {
		dup2(saved_stdout, STDOUT_FILENO);
		close(saved_stdout);
		saved_stdout = -1;
	}
}

/* xorshift, so runs are reproducible from a seed regardless of libc */
static uint32_t rng_state = 1;
static uint32_t rnd(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

static char *contentOf(struct buffer *buf) {
	size_t len;
	char *raw = rowsToString(buf, &len);
	char *out = xmalloc(len + 1);
	memcpy(out, raw, len);
	out[len] = '\0';
	free(raw);
	return out;
}

/* ---- the command set --------------------------------------------- */

/* Non-prompting commands only: anything that calls editorPrompt or
 * readKey would block on the stubbed key source, and anything that
 * touches files, subprocesses or windows is out of scope for a buffer
 * invariant check. */
static const struct {
	int cmd;
	const char *name;
} OPS[] = {
	{ CMD_FORWARD_CHAR, "forward-char" },
	{ CMD_BACKWARD_CHAR, "backward-char" },
	{ CMD_NEXT_LINE, "next-line" },
	{ CMD_PREV_LINE, "prev-line" },
	{ CMD_PAGE_UP, "page-up" },
	{ CMD_PAGE_DOWN, "page-down" },
	{ CMD_SCROLL_UP, "scroll-up" },
	{ CMD_SCROLL_DOWN, "scroll-down" },
	{ CMD_HOME, "home" },
	{ CMD_END, "end" },
	{ CMD_BEG_OF_FILE, "beg-of-file" },
	{ CMD_END_OF_FILE, "end-of-file" },
	{ CMD_FORWARD_WORD, "forward-word" },
	{ CMD_BACKWARD_WORD, "backward-word" },
	{ CMD_FORWARD_PARA, "forward-para" },
	{ CMD_BACKWARD_PARA, "backward-para" },
	{ CMD_FORWARD_SEXP, "forward-sexp" },
	{ CMD_BACKWARD_SEXP, "backward-sexp" },
	{ CMD_SENTENCE_FORWARD, "sentence-forward" },
	{ CMD_SENTENCE_BACKWARD, "sentence-backward" },
	{ CMD_RECENTER, "recenter" },
	{ CMD_NEWLINE, "newline" },
	{ CMD_BACKSPACE, "backspace" },
	{ CMD_DELETE, "delete" },
	{ CMD_KILL_LINE, "kill-line" },
	{ CMD_KILL_LINE_BACKWARDS, "kill-line-backwards" },
	{ CMD_NEWLINE_INDENT, "newline-and-indent" },
	{ CMD_OPEN_LINE, "open-line" },
	{ CMD_DELETE_WORD, "delete-word" },
	{ CMD_BACKSPACE_WORD, "backspace-word" },
	{ CMD_UPCASE_WORD, "upcase-word" },
	{ CMD_DOWNCASE_WORD, "downcase-word" },
	{ CMD_CAPCASE_WORD, "capcase-word" },
	{ CMD_TRANSPOSE_CHARS, "transpose-chars" },
	{ CMD_TRANSPOSE_WORDS, "transpose-words" },
	{ CMD_TRANSPOSE_SENTENCES, "transpose-sentences" },
	{ CMD_KILL_SEXP, "kill-sexp" },
	{ CMD_KILL_PARA, "kill-para" },
	{ CMD_MARK_PARA, "mark-para" },
	{ CMD_UNINDENT, "unindent" },
	{ CMD_SELF_INSERT, "self-insert" },
	{ CMD_TAB, "tab" },
	{ CMD_SET_MARK, "set-mark" },
	{ CMD_SWAP_MARK, "swap-mark" },
	{ CMD_MARK_BUFFER, "mark-buffer" },
	{ CMD_CUT, "cut" },
	{ CMD_COPY, "copy" },
	{ CMD_YANK, "yank" },
	{ CMD_YANK_POP, "yank-pop" },
	{ CMD_KILL_REGION, "kill-region" },
	{ CMD_UPCASE_REGION, "upcase-region" },
	{ CMD_DOWNCASE_REGION, "downcase-region" },
	{ CMD_COPY_RECT, "copy-rect" },
	{ CMD_KILL_RECT, "kill-rect" },
	{ CMD_YANK_RECT, "yank-rect" },
	{ CMD_TOGGLE_RECT_MODE, "toggle-rect-mode" },
	{ CMD_VISUAL_LINE_MODE, "visual-line-mode" },
	{ CMD_WHAT_CURSOR, "what-cursor" },
	{ CMD_UNDO, "undo" },
	{ CMD_REDO, "redo" },
};
#define NOPS ((int)(sizeof(OPS) / sizeof(OPS[0])))

/* Printable bytes for self-insert, plus some multi-byte characters so
 * the UTF-8 invariant has something to bite on. */
static const char *INSERT_CHARS = "ab z.(),\t\"'";

struct step {
	int op;	  /* index into OPS */
	int uarg; /* universal argument, 0 = none */
	int ch;	  /* self-insert byte */
	int uni;  /* if non-zero, insert this multi-byte char instead */
};

static const char *UNICHARS[] = { "\xc3\xa9", "\xe6\x97\xa5",
				  "\xf0\x9f\x98\x80" };

/* ---- invariants --------------------------------------------------- */

static char fail_reason[256];

static int checkInvariants(struct buffer *buf) {
	for (int i = 0; i < buf->numrows; i++) {
		if (!utf8_validate(buf->row[i].chars, buf->row[i].size)) {
			snprintf(fail_reason, sizeof(fail_reason),
				 "row %d is not valid UTF-8", i);
			return 0;
		}
	}
	if (buf->numrows < 1) {
		snprintf(fail_reason, sizeof(fail_reason),
			 "numrows %d violates numrows >= 1", buf->numrows);
		return 0;
	}
	/* cy < numrows (#105): every cursor position names a real row.
	 * The virtual EOF line is gone, so cy == numrows is a failure
	 * rather than a case to be tolerated. */
	if (buf->cy < 0 || buf->cy >= buf->numrows) {
		snprintf(fail_reason, sizeof(fail_reason),
			 "cy %d out of bounds (numrows %d)", buf->cy,
			 buf->numrows);
		return 0;
	}
	if (buf->cx < 0 || buf->cx > buf->row[buf->cy].size) {
		snprintf(fail_reason, sizeof(fail_reason),
			 "cx %d out of bounds (row size %d)", buf->cx,
			 buf->row[buf->cy].size);
		return 0;
	}
	if (buf->cx < buf->row[buf->cy].size &&
	    utf8_isCont(buf->row[buf->cy].chars[buf->cx])) {
		snprintf(fail_reason, sizeof(fail_reason),
			 "cursor at (%d,%d) is mid-character", buf->cx,
			 buf->cy);
		return 0;
	}
	/* A file buffer ends in a newline -- its last row is empty --
	 * unless it is empty, which has no lines to terminate.  The
	 * mutation layer maintains this, so it holds after every
	 * operation, not merely at save.  This is the check that keeps
	 * a future edit path from quietly dropping the terminator: the
	 * consequence would otherwise show up only as a file on disk. */
	if (!bufferIsEmpty(buf) && buf->row[buf->numrows - 1].size != 0) {
		snprintf(fail_reason, sizeof(fail_reason),
			 "last row \"%.20s\" is not empty: buffer does not "
			 "end in a newline",
			 (const char *)buf->row[buf->numrows - 1].chars);
		return 0;
	}
	return 1;
}

/* ---- running a sequence ------------------------------------------- */

static const char *START_LINES[] = { "alpha beta", "(gamma delta).",
				     "  indented", "", "epsilon" };
#define NSTART ((int)(sizeof(START_LINES) / sizeof(START_LINES[0])))

/* Returns 0 on success, non-zero on invariant violation.  fail_reason
 * describes the failure. */
static int runSequence(const struct step *steps, int n) {
	initTestEditor();
	muteStdout();

	struct buffer *buf = make_test_buffer_lines(START_LINES, NSTART);
	char *original = contentOf(buf);
	int rc = 0;

	for (int i = 0; i < n && rc == 0; i++) {
		E.uarg = steps[i].uarg;
		if (OPS[steps[i].op].cmd == CMD_SELF_INSERT) {
			if (steps[i].uni) {
				const char *u = UNICHARS[steps[i].uni - 1];
				E.nunicode = (int)strlen(u);
				memcpy(E.unicode, u, (size_t)E.nunicode);
				processKeypress(CMD_UNICODE);
			} else {
				E.self_insert_key = steps[i].ch;
				processKeypress(CMD_SELF_INSERT);
			}
		} else {
			processKeypress(OPS[steps[i].op].cmd);
		}

		if (!checkInvariants(buf)) {
			rc = 1;
			break;
		}
	}

	if (rc == 0) {
		for (int k = 0; k < 4096 && buf->undo != NULL; k++) {
			processKeypress(CMD_UNDO);
			if (!checkInvariants(buf)) {
				rc = 1;
				break;
			}
		}
	}

	if (rc == 0 && getenv("EMIL_FUZZ_SKIP_UNDO") == NULL) {
		char *restored = contentOf(buf);
		if (strcmp(original, restored) != 0) {
			snprintf(fail_reason, sizeof(fail_reason),
				 "undo did not restore: %zu bytes vs %zu",
				 strlen(original), strlen(restored));
			rc = 1;
		}
		free(restored);
	}

	free(original);
	clearText(&E.kill);
	cleanupTestEditor();
	unmuteStdout();
	return rc;
}

static void printSequence(const struct step *steps, int n) {
	for (int i = 0; i < n; i++) {
		printf("    %2d. %s", i + 1, OPS[steps[i].op].name);
		if (OPS[steps[i].op].cmd == CMD_SELF_INSERT) {
			if (steps[i].uni)
				printf(" <U+multibyte>");
			else
				printf(" '%c'", steps[i].ch);
		}
		if (steps[i].uarg)
			printf(" (uarg %d)", steps[i].uarg);
		printf("\n");
	}
}

/* Drop operations one at a time for as long as the failure survives. */
static int deltaDebug(struct step *steps, int n) {
	int changed = 1;
	while (changed && n > 1) {
		changed = 0;
		for (int i = 0; i < n; i++) {
			struct step trial[512];
			int m = 0;
			for (int j = 0; j < n; j++)
				if (j != i)
					trial[m++] = steps[j];
			if (runSequence(trial, m) != 0) {
				memcpy(steps, trial,
				       sizeof(struct step) * (size_t)m);
				n = m;
				changed = 1;
				break;
			}
		}
	}
	return n;
}

int main(int argc, char **argv) {
	int iterations = (argc > 1) ? atoi(argv[1]) : 2000;
	rng_state = (argc > 2) ? (uint32_t)strtoul(argv[2], NULL, 10) : 12345u;
	if (rng_state == 0)
		rng_state = 1;

	printf("fuzzing %d sequences over %d commands (seed %u)\n", iterations,
	       NOPS, rng_state);

	const char *mf = getenv("EMIL_FUZZ_MAX_FAILURES");
	int max_failures = mf ? atoi(mf) : 5;
	int failures = 0;
	for (int it = 0; it < iterations; it++) {
		int n = 2 + (int)(rnd() % 60);
		struct step steps[64];
		for (int i = 0; i < n; i++) {
			steps[i].op = (int)(rnd() % (uint32_t)NOPS);
			steps[i].uarg =
				(rnd() % 5 == 0) ? (int)(rnd() % 6) + 1 : 0;
			steps[i].ch =
				INSERT_CHARS[rnd() % strlen(INSERT_CHARS)];
			steps[i].uni = (rnd() % 6 == 0) ? (int)(rnd() % 3) + 1 :
							  0;
		}

		if (runSequence(steps, n) != 0) {
			char kept[256];
			emil_strlcpy(kept, fail_reason, sizeof(kept));
			printf("\n*** FAILURE (iteration %d, %d ops): %s\n", it,
			       n, kept);
			int m = getenv("EMIL_FUZZ_NO_REDUCE") ?
					n :
					deltaDebug(steps, n);
			runSequence(steps, m);
			printf("  reduced to %d op(s): %s\n", m, fail_reason);
			printSequence(steps, m);
			failures++;
			if (failures >= max_failures) {
				printf("\nstopping after %d failures\n",
				       max_failures);
				return 1;
			}
		}
	}

	printf("%d failure(s)\n", failures);
	return failures != 0;
}
