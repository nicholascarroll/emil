/* emil_subprocess.h — Minimal subprocess API for pipe.c.
 *
 * Spawn a child process with three pipes (stdin, stdout, stderr)
 * wired to parent-side FILE*s.  Implemented over posix_spawn so the
 * same code works on POSIX systems and on MSYS2/Cygwin where
 * posix_spawn maps to native CreateProcess.
 *
 * Isolated in its own translation unit so that platform-specific
 * issues — of which spawn/fork/exec semantics are a perennial source —
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
};

int subprocess_create(const char *const command_line[], int options,
		      struct subprocess_s *const out_process);
FILE *subprocess_stdin(const struct subprocess_s *const process);
FILE *subprocess_stdout(const struct subprocess_s *const process);
int subprocess_join(struct subprocess_s *const process,
		    int *const out_return_code);
int subprocess_destroy(struct subprocess_s *const process);

#endif /* EMIL_SUBPROCESS_H */
