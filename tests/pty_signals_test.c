/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* pty_signals_test.c: who owns the terminal, asserted through a real
 * pseudo-terminal.
 *
 * The unit suites cannot check any of this: stubs.c replaces main.o, so
 * no unit test links a line of termios code.  Whether the editor still
 * owns the terminal, and whether it hands it back on suspend or on a
 * fatal signal, is only observable from the master side of a pty.
 *
 * Needs more than a pty.  Every assertion here reads the editor's
 * termios through the master, which is a Linux and BSD convenience and
 * not portable -- on illumos a pty is a STREAMS device whose master is
 * a bare stream with no terminal semantics of its own, and FreeBSD does
 * not reflect the slave's settings either.  It also needs real job
 * control: a host runtime that swallows SIGTSTP, SIGSEGV or SIGBUS
 * makes every scenario report the runtime rather than emil.
 *
 * So this binary is built for Linux and macOS and nowhere else, decided
 * once in tests/run_tests.sh.  There is nothing conditional inside it.
 * A 57-line runtime probe used to answer "can this platform observe the
 * slave's termios" and turn a whole scenario into a SKIP when it could
 * not; the question is now settled by which targets build this file.
 * pty_input_test.c carries the scenarios that need only a pty.
 *
 * Usage: pty_signals_test <path-to-emil>
 */

#include "pty_harness.h"

/* ---- terminal ownership (invariant 4.5) ------------------------ */

/* While the editor is running it owns the terminal in raw mode.  The
 * unit suites cannot check this at all: stubs.c replaces main.o, so
 * nothing there ever touches termios.  Only a real pty can see it.
 *
 * The suspend paths raise SIGTSTP after handing the tty back, and
 * POSIX discards a stop signal sent to a member of an orphaned
 * process group -- which is exactly what spawnEmil() creates, and
 * what an editor invoked as EDITOR/GIT_EDITOR from a daemon, a CI
 * runner or a job-control-less shell runs in.  When the stop is
 * discarded the editor keeps running, and before the fix it kept
 * running in cooked mode with ECHO and ISIG restored: the next
 * Ctrl-C killed it past the unsaved-changes prompt and past atexit.
 * */

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
		 * of an orphaned process group -- which is what
		 * spawnEmil() creates -- is discarded, and Linux and
		 * illumos do that; Cygwin/MSYS2 stops the process
		 * anyway.  A stopped editor is *supposed* to have handed
		 * the terminal back, so asserting raw mode there would
		 * be asserting the opposite of correct behaviour. */
		switch (childState(&c)) {
		case CHILD_GONE:
			expect(0, "editor exited on suspend");
			break;

		case CHILD_STOPPED:
			/* The stop took.  Cooked mode is right, and what
			 * matters is that the editor reclaims the
			 * terminal on resume -- the same repair path,
			 * reached the ordinary way. */
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
 * binding -- with ISIG cleared the byte is read and reported like any
 * other -- so a live editor here is the whole assertion.  This is the
 * portable half of the pair, and on platforms where the master cannot
 * report the slave's state it is the only cover the suspend paths
 * have. */
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
			/* spawnEmil() calls setsid(), so the editor is a
			 * session leader in a new and therefore orphaned
			 * process group, and POSIX requires a stop signal
			 * sent there to be discarded.  Reaching this arm
			 * means the premise the scenario is built on no
			 * longer holds, which is a finding rather than a
			 * reason to assert nothing. */
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

/* ---- crash paths (invariant 4.5 under a fatal signal) ---------- */

/* A crash must hand the terminal back before dying.
 *
 * Without a handler the default action kills the process outright,
 * leaving the tty in raw mode on the alternate screen: the shell the
 * user drops back into echoes nothing and interprets nothing, and the
 * only way out is to type `reset` blind.  The pty here is the same
 * kind of persistent object as that shell's tty -- the settings
 * outlive the process that made them, which is exactly why this is a
 * user-visible bug and not merely untidy.
 *
 * Three things are asserted, and all three are needed:
 *   1. cooked mode is back (ECHO/ICANON/ISIG on) -- the shell works;
 *   2. the alternate screen was exited -- the user's scrollback is
 *      back;
 *   3. the process still died *of that signal* -- so the shell still
 *      reports the crash and the kernel still writes a core.  A
 *      handler that tidied up and called exit(1) would pass 1 and 2
 *      while quietly hiding every crash from the user and from any
 *      core-dump-based debugging, so 3 is the assertion that keeps
 *      the fix honest.
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
	 * forever rather than fail -- a hung CI runner instead of a
	 * red one.  Skip rather than hang, and say why.
 */

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
