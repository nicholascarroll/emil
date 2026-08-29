/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* pty_input_test.c: escape-sequence and input handling, driven through
 * a real pseudo-terminal.
 *
 * Covers what the unit tests in test_decoder.c cannot: real timing (the
 * Meta-prefix indefinite wait versus the in-flight sequence timeout),
 * interaction with the main loop's key batching and status-message
 * lifecycle, burst detection and undo granularity, and the historical
 * leaked-bytes regressions (F12 panic key, SS3 finals typed into the
 * buffer, lone-ESC-then-sequence leaking its body).
 *
 * Needs a pty and nothing else, so it runs on every target that has
 * one.  Assertions about who owns the terminal live in
 * pty_signals_test.c, which needs more than a pty.
 *
 * Usage: pty_input_test <path-to-emil>
 */

#include "pty_harness.h"

/* ---- scenarios ------------------------------------------------- */


/* A lone ESC before an arrow must not leak the arrow's body. */
static void scenarioLoneEscThenArrow(void) {
	struct child c;
	begin("lone ESC then Up arrow");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "line1\rline2", 200);
		sendStr(&c, "\033", 300); /* lone ESC keypress */
		sendStr(&c, "\033[A", 150);
		capReset();
		sendStr(&c, "X", 400);
		expect(contains(stripped(), "line1X"),
		       "arrow did not act as Up");
		expect(!contains(stripped(), "[A"),
		       "sequence body leaked as text");
		reap(&c);
	}
	finish();
}

/* ESC waits indefinitely: slow ESC, f must be Meta-f. */
static void scenarioMetaPrefixHumanSpeed(void) {
	struct child c;
	begin("slow ESC, f acts as M-f");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "alpha beta", 200);
		sendStr(&c, "\001", 150); /* C-a: line start */
		sendStr(&c, "\033", 400); /* human-speed Meta prefix */
		sendStr(&c, "f", 150);
		capReset();
		sendStr(&c, "X", 400);
		expect(contains(stripped(), "alphaX beta"),
		       "M-f did not move by word");
		reap(&c);
	}
	finish();
}

/* Unmapped sequences are consumed and reported, never typed. */
static void scenarioUnknownReported(const char *label,
				    const char *sequence,
				    const char *message) {
	struct child c;
	begin(label);
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "##", 200);
		capReset();
		sendStr(&c, sequence, 400);
		expect(childAlive(&c), "editor died");
		expect(contains(cap, message), "status message missing");
		capReset();
		sendStr(&c, "x", 400);
		expect(contains(stripped(), "##x"),
		       "following keypress swallowed");
		reap(&c);
	}
	finish();
}

/* Alt+[ has no binding and there is no sequence timeout, so the
 * following keystroke completes the CSI and is consumed and reported
 * rather than typed.  This is the original CSI behavior, restored
 * deliberately: correctness on split sequences is worth more than the
 * ergonomics of an unbound key. */
static void scenarioAltBracketConsumesNext(void) {
	struct child c;
	begin("Alt+[ consumes the next keystroke");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "##", 200);
		capReset();
		sendStr(&c, "\033[", 300);
		expect(!contains(cap, "M-["),
		       "reported before the sequence completed");
		capReset();
		sendStr(&c, "x", 400); /* final byte: completes the CSI */
		expect(contains(cap, "M-[ x"),
		       "completed sequence not reported");
		expect(!contains(stripped(), "##x"),
		       "final byte leaked into the buffer as text");
		capReset();
		sendStr(&c, "y", 400);
		expect(contains(stripped(), "##y"),
		       "keypress after the sequence swallowed");
		reap(&c);
	}
	finish();
}

/* THE guarantee bought by having no sequence timeout: a sequence
 * split by a slow transport (laggy link, slow serial line, TCP
 * retransmit) still decodes correctly no matter how long the gap.
 * Under any finite budget the tail of this arrow key would have been
 * typed into the buffer as the letter A.  This test fails under any
 * timed policy and passes only while sequence bytes block. */
static void scenarioSlowSplitSequence(void) {
	struct child c;
	struct timespec gap;
	begin("sequence split by 400ms still decodes");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "line1\rline2", 300);
		ssize_t w = write(c.mfd, "\033[", 2);
		(void)w;
		gap.tv_sec = 0;
		gap.tv_nsec = 400 * 1000000; /* far beyond any budget */
		nanosleep(&gap, NULL);
		w = write(c.mfd, "A", 1); /* completes the Up arrow */
		(void)w;
		pump(c.mfd, 300);
		capReset();
		sendStr(&c, "X", 400);
		expect(contains(stripped(), "line1X"),
		       "split arrow did not act as Up");
		expect(!contains(stripped(), "line2A"),
		       "sequence tail leaked into the buffer as text");
		reap(&c);
	}
	finish();
}

/* The frozen contract: representative mapped keys still act. */
static void scenarioMappedKeys(void) {
	struct child c;

	begin("CSI H Home and \\e[4~ End");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "abc", 200);
		sendStr(&c, "\033[H", 150);
		sendStr(&c, "1", 150);
		sendStr(&c, "\033[4~", 150);
		capReset();
		sendStr(&c, "2", 400);
		expect(contains(stripped(), "1abc2"),
		       "Home/End variants misdecoded");
		reap(&c);
	}
	finish();

	begin("SS3 \\eOH Home");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "one two", 200);
		sendStr(&c, "\033OH", 150);
		capReset();
		sendStr(&c, "Y", 400);
		expect(contains(stripped(), "Yone two"),
		       "SS3 Home misdecoded");
		reap(&c);
	}
	finish();

	begin("fast M-b backward word");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "word word", 200);
		sendStr(&c, "\033b", 150);
		capReset();
		sendStr(&c, "Z", 400);
		expect(contains(stripped(), "word Zword"),
		       "M-b misdecoded");
		reap(&c);
	}
	finish();
}

/* Multi-byte input is untouched by the escape path. */
static void scenarioUtf8Typing(void) {
	struct child c;
	begin("UTF-8 typing intact");
	if (spawnEmil(&c) == 0) {
		capReset();
		sendStr(&c, "\xe4\xbd\xa0\xe5\xa5\xbd", 500);
		expect(contains(cap, "\xe4\xbd\xa0\xe5\xa5\xbd"),
		       "CJK input mangled");
		reap(&c);
	}
	finish();
}


/* A multi-line paste undoes in one step, and its CRs become real
 * newlines with no stray ^M.
 *
 * The CR half of this is the regression that matters: an earlier
 * attempt at this feature inserted the payload as raw bytes, bypassing
 * the dispatch layer that maps CR to CMD_NEWLINE, so every pasted line
 * break arrived as a literal ^M.  Burst detection keeps every byte on
 * the normal dispatch path, so the mapping still applies -- but only a
 * test that sends CR can show that. */
static void scenarioBurstPasteUndoesInOneStep(void) {
	struct child c;
	begin("burst: multi-line paste, one undo, no ^M");
	if (spawnEmil(&c) == 0) {
		sendStr(&c, "keep", 250);
		/* 5 lines x 40 chars: far past UNDO_MERGE_LIMIT if this
		 * were treated as typing. */
		sendPaste(&c,
			  "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r"
			  "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb\r"
			  "cccccccccccccccccccccccccccccccccccccccc\r"
			  "dddddddddddddddddddddddddddddddddddddddd\r"
			  "eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
			  700);
		expect(contains(stripped(), "aaaaaaaaaa"), "paste missing");
		expect(contains(stripped(), "eeeeeeeeee"),
		       "last pasted line missing");
		expect(!contains(stripped(), "^M"),
		       "CR inserted literally instead of becoming a newline");
		capReset();
		sendStr(&c, "\037", 600); /* C-_ : one undo */
		expect(!contains(stripped(), "aaaaaaaaaa"),
		       "one undo did not remove the whole paste");
		expect(!contains(stripped(), "eeeeeeeeee"),
		       "one undo left part of the paste behind");
		expect(contains(stripped(), "keep"),
		       "undo removed text typed before the paste");
		reap(&c);
	}
	finish();
}

/* Typing is still chopped into recoverable steps.  The cap is lifted
 * only for a burst; keys that arrive one at a time must keep the
 * UNDO_MERGE_LIMIT behaviour, or the exemption has eaten the rule it
 * was supposed to leave alone. */
static void scenarioTypingStillCapped(void) {
	struct child c;
	begin("burst: typed keys still obey the merge cap");
	if (spawnEmil(&c) == 0) {
		/* 30 characters, each its own write with a settle gap:
		 * the drain loop sees no buffered bytes, so no burst. */
		for (int i = 0; i < 30; i++)
			sendStr(&c, "x", 40);
		expect(contains(stripped(), "xxxxxxxxxx"), "typing missing");
		capReset();
		sendStr(&c, "\037", 500); /* one undo */
		/* The cap is 20, so 30 typed chars is more than one run:
		 * one undo must leave some behind. */
		expect(contains(stripped(), "x"),
		       "one undo erased all typing: cap not applied");
		reap(&c);
	}
	finish();
}

/* A burst closes its run, so text typed afterwards is a separate undo
 * step rather than folding into the paste. */
static void scenarioBurstClosesRun(void) {
	struct child c;
	begin("burst: run closes when the burst ends");
	if (spawnEmil(&c) == 0) {
		sendPaste(&c,
			  "pasted_text_pasted_text_pasted_text_pasted",
			  600);
		sendStr(&c, "Z", 400); /* typed after the burst */
		capReset();
		sendStr(&c, "\037", 500); /* one undo */
		expect(!contains(stripped(), "Z"),
		       "undo did not remove the character typed after");
		expect(contains(stripped(), "pasted_text"),
		       "undo swallowed the paste as well as the typing");
		reap(&c);
	}
	finish();
}

int main(int argc, char **argv) {
	if (!ptyBegin("pty_input_test", argc, argv))
		return 0;

	scenarioLoneEscThenArrow();
	scenarioMetaPrefixHumanSpeed();
	scenarioUnknownReported("unknown key reported: F5", "\033[15~",
				"M-[ 1 5 ~");
	scenarioUnknownReported("unknown key reported: Insert", "\033[2~",
				"M-[ 2 ~");
	scenarioUnknownReported("unknown key reported: Ctrl-Right",
				"\033[1;5C", "M-[ 1 ; 5 C");
	scenarioUnknownReported("unknown key reported: F1 (SS3)", "\033OP",
				"M-O P");
	scenarioAltBracketConsumesNext();
	scenarioSlowSplitSequence();
	scenarioMappedKeys();
	scenarioUtf8Typing();
	scenarioBurstPasteUndoesInOneStep();
	scenarioTypingStillCapped();
	scenarioBurstClosesRun();

	return ptyEnd("pty_input_test");
}
