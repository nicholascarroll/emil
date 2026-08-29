/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* Invariant fuzzer for emil's buffer/undo layer.
 *
 * An ordinary suite, listed in SUITES and scored by its exit status
 * like every other.  It was a special case in run_tests.sh for as long
 * as it was invoked as a bare binary with a sequence count and a seed
 * on the command line: sixty lines of shell to loop over seeds, its own
 * verdict-by-grep, a build-only branch, and three environment knobs
 * nothing set.  One RUN_TEST per seed does the same work and needs none
 * of it -- and being named test_* means the Genode and Asterinas
 * runners, which glob for that, now run it too.
 *
 * Drives random sequences of real commands through processKeypress and
 * checks, after every operation:
 *
 *   - the flattened text is valid UTF-8 and ends in a newline
 *   - point converts to an in-bounds offset on a character boundary
 *   - bufOffset()/bufPos() round trip, and bufTextLen() agrees with
 *     the flattened text
 *   - every row is valid UTF-8
 *   - the cursor is in bounds and on a character boundary
 *   - the buffer ends in a newline unless it is empty
 *
 * and, after the whole sequence:
 *
 *   - undoing everything restores the original bytes exactly
 *     (length plus memcmp, per design §10.1)
 *
 * A failing sequence is then delta-debugged: operations are dropped one
 * at a time for as long as the failure persists, which usually reduces a
 * sixty-operation sequence to two or three.
 *
 * Sequence count and seed set are compile-time, not environment.  The
 * seeds are fixed rather than time-based on purpose: a suite that fails
 * only on some days is worse than one that misses a bug, because nobody
 * trusts it.  And there are eight rather than one because a single seed
 * is a single walk through the state space -- a heap-buffer-overflow in
 * clampCursorToViewport survived as long as it did because seed 1
 * happens not to reach it while roughly half its neighbours do.
 *
 * Override for a deeper soak with -DFUZZ_SEQS=200000.
 */
#ifndef FUZZ_SEQS
#define FUZZ_SEQS 10000
#endif

/* Reported failures per seed, so one broken invariant does not print a
 * thousand reduced sequences. */
#define FUZZ_MAX_FAILURES 5

#include "test.h"
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

/* muteStdout/unmuteStdout come from test_harness.h. */

/* xorshift, so runs are reproducible from a seed regardless of libc */
static uint32_t rng_state = 1;
static uint32_t rnd(void) {
	rng_state ^= rng_state << 13;
	rng_state ^= rng_state >> 17;
	rng_state ^= rng_state << 5;
	return rng_state;
}

/* ---- flat oracle (design §10.1) ----
 *
 * G0 stores a buffer as one contiguous byte array, so the undo oracle
 * becomes a length comparison and a memcmp rather than a walk over
 * rows.  Taking the flat form now means the oracle already speaks the
 * post-G0 model before the storage changes under it.
 *
 * Length-and-memcmp rather than strcmp: strcmp stops at the first NUL
 * byte, so a mutation that introduced one would compare equal to a
 * buffer truncated at that point.  Null bytes are rejected at load
 * (design §8.1) and so should never appear, which is exactly why the
 * oracle should be the thing that notices if one does. */
struct snapshot {
	uint8_t *bytes;
	size_t len;
};

static struct snapshot contentOf(struct buffer *buf) {
	struct snapshot s;
	char *raw = rowsToString(buf, &s.len);
	s.bytes = xmalloc(s.len + 1);
	memcpy(s.bytes, raw, s.len);
	s.bytes[s.len] = '\0';
	free(raw);
	return s;
}

static int snapshotEqual(const struct snapshot *a, const struct snapshot *b) {
	return a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

/* Offset of the first differing byte, for the failure message.  Equal
 * snapshots never reach this. */
static size_t firstDifference(const struct snapshot *a,
			      const struct snapshot *b) {
	size_t n = a->len < b->len ? a->len : b->len;
	for (size_t i = 0; i < n; i++)
		if (a->bytes[i] != b->bytes[i])
			return i;
	return n;
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

/* ---- flat invariants (design §2.4) ----
 *
 * The row-model checks below stay: they are still true, and during the
 * migration they catch a row array that has drifted from the text it
 * is supposed to represent.  These flat checks are the ones that
 * outlive it -- each is B-1, B-2 or B-3 stated against the byte string
 * rather than against rows -- plus the round trip that every caller
 * converted to offsets depends on.
 *
 * Checking both models against each other is the point.  A defect that
 * corrupts rows but leaves the flattened text intact, or the reverse,
 * shows up here as a disagreement rather than passing both. */
static int checkFlatInvariants(struct buffer *buf) {
	struct snapshot flat = contentOf(buf);
	int ok = 1;

	/* B-2: the whole buffer is valid UTF-8, not merely each row.
	 * A row-at-a-time check cannot see a sequence broken across a
	 * row boundary; under G0 there are no boundaries to hide it. */
	if (!utf8_validate(flat.bytes, flat.len)) {
		snprintf(fail_reason, sizeof(fail_reason),
			 "flat text is not valid UTF-8 (%zu bytes)", flat.len);
		ok = 0;
	}

	/* B-1: a non-empty buffer ends in a newline. */
	if (ok && flat.len > 0 && flat.bytes[flat.len - 1] != '\n') {
		snprintf(fail_reason, sizeof(fail_reason),
			 "flat text does not end in a newline (last byte 0x%02x)",
			 flat.bytes[flat.len - 1]);
		ok = 0;
	}

	/* bufTextLen() must agree with the text it claims to measure. */
	if (ok && bufTextLen(buf) != flat.len) {
		snprintf(fail_reason, sizeof(fail_reason),
			 "bufTextLen %zu but flat text is %zu bytes",
			 bufTextLen(buf), flat.len);
		ok = 0;
	}

	if (ok) {
		size_t off = bufOffset(buf, buf->cx, buf->cy);

		/* Point is addressable within the text. */
		if (off > flat.len) {
			snprintf(fail_reason, sizeof(fail_reason),
				 "point offset %zu past end of text %zu", off,
				 flat.len);
			ok = 0;
		}
		/* B-3: point is on a character boundary in flat space.
		 * The row-model check below asserts the same thing per
		 * row; this one would still hold if rows vanished. */
		else if (off < flat.len && utf8_isCont(flat.bytes[off])) {
			snprintf(fail_reason, sizeof(fail_reason),
				 "point offset %zu is mid-character", off);
			ok = 0;
		} else {
			/* The round trip every offset-converted caller
			 * relies on.  If this fails, a caller reading
			 * through bufPos() is addressing the wrong byte
			 * while both models look individually sane. */
			int bx, by;
			bufPos(buf, off, &bx, &by);
			if (bx != buf->cx || by != buf->cy) {
				snprintf(fail_reason, sizeof(fail_reason),
					 "offset round trip: (%d,%d) -> %zu -> "
					 "(%d,%d)",
					 buf->cx, buf->cy, off, bx, by);
				ok = 0;
			}
		}
	}

	free(flat.bytes);
	return ok;
}

static int checkInvariants(struct buffer *buf) {
	if (!checkFlatInvariants(buf))
		return 0;
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
	struct snapshot original = contentOf(buf);
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

	if (rc == 0) {
		struct snapshot restored = contentOf(buf);
		if (!snapshotEqual(&original, &restored)) {
			size_t at = firstDifference(&original, &restored);
			snprintf(fail_reason, sizeof(fail_reason),
				 "undo did not restore: %zu bytes vs %zu, "
				 "first difference at offset %zu",
				 original.len, restored.len, at);
			rc = 1;
		}
		free(restored.bytes);
	}

	free(original.bytes);
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

/* One seed, FUZZ_SEQS random sequences.  Reports through the normal
 * assertion path, so a failure prints the reduced sequence and the
 * suite's exit status carries the verdict. */
static void fuzzSeed(uint32_t seed) {
	rng_state = seed ? seed : 1;
	int failures = 0;

	for (int it = 0; it < FUZZ_SEQS; it++) {
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
			printf("  FAIL: %s:%d [seed %u]: iteration %d, "
			       "%d ops: %s\n",
			       __FILE__, __LINE__, (unsigned)seed, it, n, kept);
			int m = deltaDebug(steps, n);
			runSequence(steps, m);
			printf("    reduced to %d op(s): %s\n", m,
			       fail_reason);
			printSequence(steps, m);
			_current_test_failed = 1;
			if (++failures >= FUZZ_MAX_FAILURES) {
				printf("    stopping after %d failures\n",
				       FUZZ_MAX_FAILURES);
				return;
			}
		}
	}
}

void setUp(void) {}
void tearDown(void) {}

static void test_seed_1(void) { fuzzSeed(1); }
static void test_seed_2(void) { fuzzSeed(2); }
static void test_seed_3(void) { fuzzSeed(3); }
static void test_seed_4(void) { fuzzSeed(4); }
static void test_seed_5(void) { fuzzSeed(5); }
static void test_seed_6(void) { fuzzSeed(6); }
static void test_seed_7(void) { fuzzSeed(7); }
static void test_seed_8(void) { fuzzSeed(8); }

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_seed_1);
	RUN_TEST(test_seed_2);
	RUN_TEST(test_seed_3);
	RUN_TEST(test_seed_4);
	RUN_TEST(test_seed_5);
	RUN_TEST(test_seed_6);
	RUN_TEST(test_seed_7);
	RUN_TEST(test_seed_8);

	return TEST_END();
}
