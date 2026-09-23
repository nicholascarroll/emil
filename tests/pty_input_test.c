/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* pty_input_test.c: escape-sequence and input handling, driven through
 * a real pseudo-terminal.
 *
 * Covers what the unit tests in test_decoder.c cannot: real timing (the
 * Meta-prefix indefinite wait versus the in-flight sequence timeout),
 * interaction with the main loop's key batching and status-message
 * lifecycle, burst detection and undo granularity, and the historical
 * leaked-bytes regressions (F12 panic key, SS3 finals typed into the
 * buffer, lone-ESC-then-sequence leaking its body).  Also which buffer
 * a keystroke edits: window focus across popups on a split screen, and
 * the commands a prompt must refuse.
 *
 * Needs a pty and nothing else, so it runs on every target that has
 * one.  Assertions about who owns the terminal live in
 * pty_signals_test.c, which needs more than a pty.
 *
 * Usage: pty_input_test <path-to-emil>
 */

#include "pty_harness.h"
#include <sys/stat.h>

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

/* ---- which buffer a keystroke edits ---------------------------- *
 *
 * These scenarios need files on disk, a split screen and a way to tell
 * which window is focused.  They live here rather than in a binary of
 * their own because they need a pty and nothing more (see
 * pty_harness.h on why that is the only split). */

/* A scratch directory holding a.txt, b.txt, c1.txt and c2.txt.  The
 * last two share a prefix, so completing "c" offers a list.  Contents
 * never mention a file name, so a name in the capture comes from a
 * status bar, the prompt or the completion list. */
static char fdir[64];

static void writeFile(const char *name, const char *text) {
	char path[128];
	snprintf(path, sizeof(path), "%s/%s", fdir, name);
	FILE *f = fopen(path, "w");
	if (f) {
		fputs(text, f);
		fclose(f);
	}
}

/* mkdtemp is POSIX.1-2008, beyond the _XOPEN_SOURCE 600 this harness
 * asks for; reserve a unique name with mkstemp and reuse it. */
static int makeFocusDir(void) {
	snprintf(fdir, sizeof(fdir), "/tmp/emil_pty_XXXXXX");
	int fd = mkstemp(fdir);
	if (fd < 0)
		return -1;
	close(fd);
	unlink(fdir);
	if (mkdir(fdir, 0700) != 0)
		return -1;
	writeFile("a.txt", "alpha\n");
	writeFile("b.txt", "bravo\n");
	writeFile("c1.txt", "charlie\n");
	writeFile("c2.txt", "charlie two\n");
	return 0;
}

static void removeFocusDir(void) {
	const char *names[] = { "a.txt", "b.txt", "c1.txt", "c2.txt" };
	char path[128];
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		snprintf(path, sizeof(path), "%s/%s", fdir, names[i]);
		unlink(path);
	}
	rmdir(fdir);
}

static char *focusPath(const char *name) {
	static char path[128];
	snprintf(path, sizeof(path), "%s/%s", fdir, name);
	return path;
}

/* How the last status bar drawn for `name` shows its window: 1
 * focused, 0 unfocused, -1 not drawn since the last capReset().  A bar
 * is the name, a space, two flag columns (-, *, %), then the fill,
 * which is ' ' for the focused window and '-' for the others. */
static int barFocused(const char *name) {
	const char *s = stripped();
	size_t n = strlen(name);
	int state = -1;
	for (const char *p = strstr(s, name); p; p = strstr(p + 1, name)) {
		const char *q = p + n;
		if (q[0] != ' ')
			continue;
		if (q[1] == 0 || !strchr("-*%", q[1]))
			continue;
		if (q[2] == 0 || !strchr("-*%", q[2]))
			continue;
		if (q[3] == ' ')
			state = 1;
		else if (q[3] == '-')
			state = 0;
	}
	return state;
}

/* Pump until `text` is in the capture or max_ms has passed (scaled,
 * like every wait here).  The scenarios below take many steps, and
 * each must finish before the next key goes in: a fixed settle time
 * that suits a native build is too short under the sanitizers, and a
 * key that arrives early lands somewhere else and fails the scenario
 * for a reason unrelated to what it tests.  Returns whether the text
 * appeared. */
static int pumpUntil(struct child *c, const char *text, int max_ms) {
	for (int waited = 0; waited < max_ms; waited += 100) {
		if (contains(stripped(), text))
			return 1;
		pump(c->mfd, 100);
	}
	return contains(stripped(), text);
}

/* Send keys and wait for `until` to be drawn in response. */
static int step(struct child *c, const char *keys, const char *until) {
	capReset();
	sendStr(c, keys, 0);
	return pumpUntil(c, until, 3000);
}

/* Split, move to the lower window, and visit b.txt there, so the two
 * windows show different files and the lower one has focus. */
static int splitVisitLower(struct child *c) {
	return step(c, "\0302", "alpha") &&	    /* C-x 2 */
	       step(c, "\030o", "alpha") &&	    /* C-x o */
	       step(c, "\030\006", "Find File:") && /* C-x C-f */
	       step(c, focusPath("b.txt"), "b.txt") && step(c, "\r", "bravo");
}

/* Open Find File with the c1/c2 completion list showing. */
static int openCompletionList(struct child *c) {
	if (!step(c, "\030\006", "Find File:") ||
	    !step(c, focusPath("c"), focusPath("c")))
		return 0;
	sendStr(c, "\t", 300);
	return step(c, "\t", "*Completions*");
}

/* C-g on a completion list, from the lower window, used to leave the
 * cursor in the upper window while keystrokes still went to the lower
 * one's buffer. */
static void scenarioCompletionCancelKeepsFocus(void) {
	struct child c;
	begin("focus: C-g on completion list keeps window");
	if (makeFocusDir() == 0 &&
	    spawnEmilOpts(&c, focusPath("a.txt"), 80, 24) == 0) {
		expect(splitVisitLower(&c), "setup: could not visit b.txt");
		expect(openCompletionList(&c),
		       "setup: completion list not shown");
		sendStr(&c, "\007", 300); /* C-g */
		expect(step(&c, "Q", "Qbravo"), "typing did not reach b.txt");
		expect(barFocused("b.txt") == 1, "b.txt's window lost focus");
		expect(barFocused("a.txt") == 0, "a.txt's window took focus");
		reap(&c);
	}
	removeFocusDir();
	finish();
}

/* Accepting from the list opened the file in the upper window. */
static void scenarioCompletionAcceptOwnWindow(void) {
	struct child c;
	begin("focus: file from completion list, own window");
	if (makeFocusDir() == 0 &&
	    spawnEmilOpts(&c, focusPath("a.txt"), 80, 24) == 0) {
		expect(splitVisitLower(&c), "setup: could not visit b.txt");
		expect(openCompletionList(&c),
		       "setup: completion list not shown");
		expect(step(&c, "1.txt", "c1.txt"), "setup: typing lost");
		expect(step(&c, "\r", "charlie"), "c1.txt was not opened");
		expect(barFocused("c1.txt") == 1,
		       "c1.txt not shown in the focused window");
		expect(barFocused("a.txt") == 0,
		       "upper window no longer shows a.txt");
		expect(barFocused("b.txt") == -1,
		       "b.txt still shown: c1.txt replaced the wrong window");
		reap(&c);
	}
	removeFocusDir();
	finish();
}

/* Both windows on one buffer; a palette symbol chosen from the lower
 * window lands at the lower window's cursor, and focus stays there. */
static void scenarioPaletteSharedBuffer(void) {
	struct child c;
	begin("focus: palette insert, split on one buffer");
	if (makeFocusDir() == 0) {
		writeFile("a.txt", "line one\nline two\nline three\n");
		if (spawnEmilOpts(&c, focusPath("a.txt"), 80, 24) == 0) {
			int ok = step(&c, "\0302", "line one") && /* C-x 2 */
				 step(&c, "\030o", "line one") && /* C-x o */
				 step(&c, "\033>", "line one") && /* M-> */
				 step(&c, "\020", "line one") &&  /* C-p */
				 step(&c, "\005", "line one") &&  /* C-e */
				 step(&c, "\033/", "Palette");	  /* M-/ */
			expect(ok, "setup: palette did not open");
			sendStr(&c, "\r", 400);
			expect(step(&c, "\030\023", "Wrote"), /* C-x C-s */
			       "setup: save did not complete");
			reap(&c);

			char text[256] = { 0 };
			FILE *f = fopen(focusPath("a.txt"), "r");
			if (f) {
				size_t n = fread(text, 1, sizeof(text) - 1, f);
				text[n] = 0;
				fclose(f);
			}
			const char *want = "line one\nline two\nline three";
			size_t wn = strlen(want);
			expect(strncmp(text, want, wn) == 0,
			       "symbol inserted before the lower cursor");
			expect(strlen(text) > wn && text[wn] != '\n',
			       "symbol not inserted at end of line three");
		}
	}
	removeFocusDir();
	finish();
}

/* Buffer-list commands typed in a prompt.  Unreachable until C-x
 * chords could reach the minibuffer; once they could, next-buffer and
 * kill-buffer searched the buffer list for the minibuffer and
 * dereferenced NULL. */
static void scenarioMinibufferRefusesBufferList(void) {
	struct child c;
	const char *refused = "Not available in the minibuffer";
	begin("minibuffer: C-x <right>, C-x k refused");
	if (makeFocusDir() == 0 &&
	    spawnEmilOpts(&c, focusPath("a.txt"), 80, 24) == 0) {
		expect(step(&c, "\030\006", "Find File:"), "setup: no prompt");
		expect(step(&c, "\030\033[C", refused), /* C-x <right> */
		       "C-x <right> not reported as refused");
		expect(childAlive(&c), "editor died on C-x <right>");
		expect(step(&c, "\030k", refused), /* C-x k */
		       "C-x k not reported as refused");
		expect(childAlive(&c), "editor died on C-x k");
		sendStr(&c, "\007", 300); /* C-g */
		expect(step(&c, "Z", "Zalpha"),
		       "a.txt no longer the buffer being edited");
		reap(&c);
	}
	removeFocusDir();
	finish();
}

/* previous-buffer in a prompt moved E.buf to a file while the prompt
 * stayed on screen, so what was typed next went into the file. */
static void scenarioMinibufferPreviousBuffer(void) {
	struct child c;
	begin("minibuffer: C-x <left> leaves files alone");
	if (makeFocusDir() == 0 &&
	    spawnEmilOpts(&c, focusPath("a.txt"), 80, 24) == 0) {
		expect(step(&c, "\030\006", "Find File:"), "setup: no prompt");
		expect(step(&c, "\030\033[D", /* C-x <left> */
			    "Not available in the minibuffer"),
		       "C-x <left> not reported as refused");
		expect(step(&c, "zzz", "Find File: zzz"),
		       "typing after C-x <left> left the prompt");
		sendStr(&c, "\007", 300); /* C-g */
		expect(step(&c, "\030\023", "No changes need to be saved"),
		       "a file was modified behind the prompt");
		reap(&c);
	}
	removeFocusDir();
	finish();
}

/* toggle-read-only in a prompt stuck to the minibuffer, which outlives
 * the prompt, so every later prompt silently refused typing. */
static void scenarioMinibufferReadOnly(void) {
	struct child c;
	begin("minibuffer: C-x C-q does not stick");
	if (makeFocusDir() == 0 &&
	    spawnEmilOpts(&c, focusPath("a.txt"), 80, 24) == 0) {
		expect(step(&c, "\030\006", "Find File:"), "setup: no prompt");
		expect(step(&c, "\030\021", /* C-x C-q */
			    "Not available in the minibuffer"),
		       "C-x C-q not reported as refused");
		sendStr(&c, "\007", 300); /* C-g */
		expect(step(&c, "\030\006", "Find File:"),
		       "setup: no second prompt");
		expect(step(&c, "abc", "Find File: abc"),
		       "the next prompt would not take typing");
		reap(&c);
	}
	removeFocusDir();
	finish();
}

/* M-! with nothing on stdout: the status line says what happened, and
 * no *Shell Output* window replaces the file (#132). */
static void scenarioShellWithoutOutput(void) {
	struct child c;
	begin("shell: no stdout, no output window");
	if (makeFocusDir() == 0 &&
	    spawnEmilOpts(&c, focusPath("a.txt"), 80, 24) == 0) {
		expect(step(&c, "\033!", "Shell:"), "setup: no shell prompt");
		expect(step(&c, "true\r",
			    "(Shell command succeeded with no output)"),
		       "no output not reported");
		expect(!contains(stripped(), "*Shell Output*"),
		       "empty output shown in a window");

		expect(step(&c, "\033!", "Shell:"), "setup: no shell prompt");
		/* The message must not appear in the command as typed,
		 * or the wait below would end before the command ran. */
		expect(step(&c, "printf 'err%s' or >&2\r", "error"),
		       "stderr not on the status line");
		expect(!contains(stripped(), "*Shell Output*"),
		       "stderr-only command opened an output window");
		expect(contains(stripped(), "alpha"), "a.txt was replaced");

		expect(step(&c, "\033!", "Shell:"), "setup: no shell prompt");
		expect(step(&c, "echo hi\r", "*Shell Output*"),
		       "stdout no longer shown");
		reap(&c);
	}
	removeFocusDir();
	finish();
}

/* Screen row of the last status bar drawn whose text contains `name`,
 * or -1.  Bars are drawn as CSI row;1H, then reverse video. */
static int barRow(const char *name) {
	int row = -1;
	for (size_t i = 0; i + 2 < cap_len; i++) {
		if (cap[i] != '\033' || cap[i + 1] != '[')
			continue;
		size_t j = i + 2;
		int r = 0;
		while (j < cap_len && cap[j] >= '0' && cap[j] <= '9')
			r = r * 10 + (cap[j++] - '0');
		if (j == i + 2 || strncmp(cap + j, ";1H\033[7m", 7) != 0)
			continue;
		j += 7;
		size_t end = j;
		while (end < cap_len && cap[end] != '\033')
			end++;
		for (size_t k = j; k + strlen(name) <= end; k++)
			if (strncmp(cap + k, name, strlen(name)) == 0) {
				row = r;
				break;
			}
	}
	return row;
}

/* Screen row the cursor was last shown at: the CSI row;col H just
 * before the sequence that makes the cursor visible.  -1 if none. */
static int cursorRow(void) {
	const char *show = "H\033[?7h\033[?25h";
	int row = -1;
	for (const char *p = strstr(cap, show); p; p = strstr(p + 1, show)) {
		const char *q = p - 1;
		while (q > cap && *q >= '0' && *q <= '9')
			q--; /* column */
		if (q <= cap || *q != ';')
			continue;
		q--;
		while (q > cap && *q >= '0' && *q <= '9')
			q--; /* row */
		if (*q == '[')
			row = atoi(q + 1);
	}
	return row;
}

/* #129: with the lower window of a split focused, M-/ drew the cursor in
 * the upper window rather than in the palette.  showPopupBuffer() had
 * moved focus to window 0 and the palette then focused its own window
 * too, so two windows claimed focus and the first one won. */
static void scenarioPaletteCursor(void) {
	struct child c;
	begin("palette: cursor drawn in the palette");
	if (makeFocusDir() == 0 &&
	    spawnEmilOpts(&c, focusPath("a.txt"), 80, 24) == 0) {
		int ok = step(&c, "\0302", "alpha") && /* C-x 2 */
			 step(&c, "\030o", "alpha") && /* C-x o */
			 step(&c, "\033/", "Palette"); /* M-/ */
		expect(ok, "setup: palette did not open");
		pump(c.mfd, 200);
		int below = barRow("a.txt"); /* the lower window's bar */
		int palette = barRow("Palette");
		int cursor = cursorRow();
		expect(below > 0 && palette > below,
		       "setup: palette window not below the split");
		expect(cursor > below && cursor < palette,
		       "cursor not in the palette window");
		reap(&c);
	}
	removeFocusDir();
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
	scenarioCompletionCancelKeepsFocus();
	scenarioCompletionAcceptOwnWindow();
	scenarioPaletteSharedBuffer();
	scenarioMinibufferRefusesBufferList();
	scenarioMinibufferPreviousBuffer();
	scenarioMinibufferReadOnly();
	scenarioShellWithoutOutput();
	scenarioPaletteCursor();

	return ptyEnd("pty_input_test");
}
