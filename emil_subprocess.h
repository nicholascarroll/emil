/* emil_subprocess.h — Minimal subprocess API for pipe.c.
 *
 * Spawn a child process with three pipes (stdin, stdout, stderr)
 * wired to parent-side FILE*s.  Implemented over posix_spawn so the
 * same code works on POSIX systems and on MSYS2/Cygwin where
 * posix_spawn maps to native CreateProcess.
 *
 * Isolated in its own translation unit so that platform-specific issues
 * can be triaged and unit-tested independently of pipe.c's editor
 * integration.  See tests/test_subprocess.c.
 *
 * Platforms without a shell should define EMIL_DISABLE_SHELL, which
 * causes this translation unit to compile to nothing. */

#ifndef EMIL_SUBPROCESS_H
#define EMIL_SUBPROCESS_H

#include <stdio.h>
#include <sys/types.h>

enum subprocess_option_e {
	subprocess_option_inherit_environment = 0x2,
	subprocess_option_search_user_path = 0x10
};

struct subprocess_s {
	FILE *stdin_file;
	FILE *stdout_file;
	FILE *stderr_file;
	pid_t child;
	int return_status;
	/* 1 if the child was spawned into its own process group (pgid ==
	 * child), so signals can reach an entire shell pipeline.  0 when
	 * POSIX_SPAWN_SETPGROUP is unavailable; subprocess_signal then
	 * degrades to signalling the immediate child only. */
	int grouped;
};

int subprocess_create(const char *const command_line[], int options,
		      struct subprocess_s *const out_process);
FILE *subprocess_stdin(const struct subprocess_s *const process);
FILE *subprocess_stdout(const struct subprocess_s *const process);
int subprocess_join(struct subprocess_s *const process,
		    int *const out_return_code);

/* Send 'sig' to the child — to its whole process group when the
 * child was spawned grouped (see struct field), so every member of a
 * shell pipeline receives it.  Returns kill()'s result; 0 if there
 * is no live child. */
int subprocess_signal(struct subprocess_s *const process, int sig);

/* Non-blocking join: 1 = reaped (exit code stored), 0 = still
 * running, -1 = error.  For cancellation paths that must not block. */
int subprocess_tryjoin(struct subprocess_s *const process,
		       int *const out_return_code);
int subprocess_destroy(struct subprocess_s *const process);

#endif /* EMIL_SUBPROCESS_H */
