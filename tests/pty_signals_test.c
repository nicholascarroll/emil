/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* pty_signals_test.c: who owns the terminal, asserted through a real
 * pseudo-terminal.
 *
 * Usage: pty_signals_test <path-to-emil>
 */

#include "pty_harness.h"

/* ---- terminal ownership invariant ------------------------ */


/*Invariant: On normal exit, emil must restore cooked mode and leave the alternate screen buffer. On fatal error, emil must make a best-effort restoration of terminal state.

Temporary cooked mode is permitted **only** when the editor remains in the alternate screen buffer and has a defined path back to raw mode. The shell drawer is the intended case: it restores cooked mode while preserving the alternate screen and relies on `SIGCONT` to restore raw mode and redraw.

A running shell command does not use cooked mode: the terminal stays raw, with only `ISIG` and the `VINTR`/`VQUIT`/`VSUSP` characters changed for the duration, and the saved settings are restored before the prompt returns.
*/
static void scenarioTerminalOwnedAfterSuspend(const char *label,
					      const char *keys) {
	struct child c;
	begin(label);
	if (spawnEmil(&c) == 0) {
		struct termios before, after;
		expect(tcgetattr(c.mfd, &before) == 0,
		       "cannot read the editor's termios");

		capReset();
		sendStr(&c, keys, 600);

		/* Whether the raise() stops the process is a property of
		 * the platform's job control, not of emil, and the
		 * correct terminal state differs between the two
		 * outcomes.  POSIX says a stop signal sent to a member
		 * of an orphaned process group is discarded, and Linux and
		 * illumos do that; Cygwin/MSYS2 stops the process
		 * anyway.  A stopped editor is *supposed* to have handed
		 * the terminal back, so asserting raw mode there would
		 * be asserting the opposite of correct behaviour. */
		switch (childState(&c)) {
		case CHILD_GONE:
			expect(0, "editor exited on suspend");
			break;

		case CHILD_STOPPED:
			kill(c.pid, SIGCONT);
			pump(c.mfd, 600);
			if (childState(&c) != CHILD_RUNNING) {
				expect(0, "editor did not resume on SIGCONT");
				break;
			}
			if (tcgetattr(c.mfd, &after) == 0) {
				expect(!(after.c_lflag & ECHO),
				       "ECHO left on after resume");
				expect(!(after.c_lflag & ISIG),
				       "ISIG left on after resume");
				expect(!(after.c_lflag & ICANON),
				       "ICANON left on after resume");
			}
			break;

		case CHILD_RUNNING:
			/* The stop was discarded and the editor ran on,
			 * so it must still own the terminal. */
			if (tcgetattr(c.mfd, &after) == 0) {
				expect(!(after.c_lflag & ECHO),
				       "ECHO left on while the editor is running");
				expect(!(after.c_lflag & ISIG),
				       "ISIG left on: Ctrl-C would kill the editor");
				expect(!(after.c_lflag & ICANON),
				       "ICANON left on while the editor is running");
			}
			break;
		}
		reap(&c);
	}
	finish();
}

/* The consequence, asserted end to end and without needing to read any
 * termios: after a discarded suspend the editor must still survive a
 * Ctrl-C, because it must still own the terminal.  Ctrl-C is not a
 * binding. */
static void scenarioCtrlCSurvivesAfterSuspend(const char *label,
					      const char *keys) {
	struct child c;
	begin(label);
	if (spawnEmil(&c) == 0) {
		sendStr(&c, keys, 600);
		switch (childState(&c)) {
		case CHILD_GONE:
			expect(0, "editor gone after suspend");
			break;
		case CHILD_STOPPED:
			expect(0, "editor stopped: the stop was not "
				  "discarded for an orphaned process group");
			break;
		case CHILD_RUNNING:
			sendStr(&c, "\003", 600); /* Ctrl-C */
			expect(childState(&c) != CHILD_GONE,
			       "editor died on Ctrl-C after suspend");
			break;
		}
		reap(&c);
	}
	finish();
}

/* ---- Fatal error invariant ---------- */

/* A crash must try to hand the terminal back before dying.
 *
 *
 * Assertions:
 *   1. cooked mode is back (ECHO/ICANON/ISIG on) and the shell works;
 *   2. the alternate screen was exited and the user's scrollback is
 *      back;
 *   3. the process still died *of that signal*, allowing the shell
 *      to report the crash and the kernel to write the core.
 *
 * The signal is sent from here rather than provoked inside the editor
 * because the handler must work for an asynchronous SIGSEGV/SIGBUS
 * and for emil's own abort() alike, and only the sent form is
 * reproducible. */
static void scenarioTerminalRestoredAfterFatalSignal(const char *label,
						     int sig) {
	struct child c;
	begin(label);

	/* These three assertions are about emil's own fatal-signal
	 * handler, and they are only meaningful when the process we
	 * signal is emil.  Under a Wasm runtime it is not: the pid
	 * belongs to the host runtime, which owns the process and
	 * installs its own handlers.  Wasmer in particular reserves
	 * SIGSEGV and SIGBUS for guard-page and trap handling and does
	 * not die of them at all, so the waitpid() below would block
	 * forever rather than fail. Skip rather than hang.*/

	if (spawnEmil(&c) == 0) {
		struct termios before, after;
		int can_see = (tcgetattr(c.mfd, &before) == 0);

		capReset();
		kill(c.pid, sig);
		pump(c.mfd, 400); /* collect the restore sequence */

		int st = 0;
		pid_t r = waitpid(c.pid, &st, 0);
		expect(r == c.pid && WIFSIGNALED(st) && WTERMSIG(st) == sig,
		       "editor did not die of the signal it was sent"
		       " (no re-raise: crash hidden from the shell and"
		       " no core dumped)");
		expect(contains(cap, "\033[?1049l"),
		       "alternate screen not exited on crash");

		expect(can_see, "cannot read the editor's termios");
		if (tcgetattr(c.mfd, &after) == 0) {
			expect(after.c_lflag & ECHO,
			       "ECHO left off after crash: shell echoes nothing");
			expect(after.c_lflag & ICANON,
			       "ICANON left off after crash: shell reads no lines");
			expect(after.c_lflag & ISIG,
			       "ISIG left off after crash: Ctrl-C does nothing");
		}

		/* Already reaped above; reap() would kill a pid that is
		 * no longer ours. */
		close(c.mfd);
	}
	finish();
}

int main(int argc, char **argv) {
	if (!ptyBegin("pty_signals_test", argc, argv))
		return 0;

	scenarioTerminalOwnedAfterSuspend("terminal owned after C-z", "\032");
	scenarioTerminalOwnedAfterSuspend("terminal owned after C-x z",
					  "\030z");
	scenarioTerminalOwnedAfterSuspend("terminal owned after C-x C-z",
					  "\030\032");
	scenarioCtrlCSurvivesAfterSuspend("Ctrl-C after C-z does not kill it",
					  "\032");
	scenarioCtrlCSurvivesAfterSuspend("Ctrl-C after C-x z does not kill it",
					  "\030z");
	scenarioCtrlCSurvivesAfterSuspend(
		"Ctrl-C after C-x C-z does not kill it", "\030\032");
	scenarioTerminalRestoredAfterFatalSignal("terminal restored on SIGSEGV",
						 SIGSEGV);
	scenarioTerminalRestoredAfterFatalSignal("terminal restored on SIGABRT",
						 SIGABRT);
	scenarioTerminalRestoredAfterFatalSignal("terminal restored on SIGBUS",
						 SIGBUS);

	return ptyEnd("pty_signals_test");
}
