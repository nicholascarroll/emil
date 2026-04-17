/* emil_subprocess.c — Implementation of the subprocess API.
 *
 * See emil_subprocess.h for the public interface.  When
 * EMIL_DISABLE_SHELL is defined, this translation unit compiles to
 * nothing. */

#ifndef EMIL_DISABLE_SHELL

/* Feature test macros must precede all system headers so that
 * posix_spawn, waitpid, fdopen etc. are declared on strict platforms
 * (OpenIndiana / Solaris). */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200112L
#endif
#ifdef __sun
#ifndef __EXTENSIONS__
#define __EXTENSIONS__ 1
#endif
#endif

#include "emil_subprocess.h"

#include <spawn.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

int subprocess_create(const char *const command_line[], int options,
		      struct subprocess_s *const out_process) {
	int stdinfd[2];	 /* parent writes [1], child reads [0] */
	int stdoutfd[2]; /* child writes [1], parent reads [0] */
	int stderrfd[2]; /* child writes [1], parent reads [0] */
	pid_t child;
	posix_spawn_file_actions_t actions;
	extern char **environ;

	memset(out_process, 0, sizeof(*out_process));

	if (pipe(stdinfd) != 0)
		return -1;

	if (pipe(stdoutfd) != 0) {
		close(stdinfd[0]);
		close(stdinfd[1]);
		return -1;
	}

	if (pipe(stderrfd) != 0) {
		close(stdinfd[0]);
		close(stdinfd[1]);
		close(stdoutfd[0]);
		close(stdoutfd[1]);
		return -1;
	}

	if (posix_spawn_file_actions_init(&actions) != 0) {
		close(stdinfd[0]);
		close(stdinfd[1]);
		close(stdoutfd[0]);
		close(stdoutfd[1]);
		close(stderrfd[0]);
		close(stderrfd[1]);
		return -1;
	}

	/* Child: close parent's ends, dup pipe ends onto std fds */
	posix_spawn_file_actions_addclose(&actions, stdinfd[1]);
	posix_spawn_file_actions_addclose(&actions, stdoutfd[0]);
	posix_spawn_file_actions_addclose(&actions, stderrfd[0]);
	posix_spawn_file_actions_adddup2(&actions, stdinfd[0], STDIN_FILENO);
	posix_spawn_file_actions_adddup2(&actions, stdoutfd[1], STDOUT_FILENO);
	posix_spawn_file_actions_adddup2(&actions, stderrfd[1], STDERR_FILENO);
	posix_spawn_file_actions_addclose(&actions, stdinfd[0]);
	posix_spawn_file_actions_addclose(&actions, stdoutfd[1]);
	posix_spawn_file_actions_addclose(&actions, stderrfd[1]);

	char *const *env = (options & subprocess_option_inherit_environment) ?
				   environ :
				   (char *const[]){ NULL };

	int rc;
	if (options & subprocess_option_search_user_path) {
		rc = posix_spawnp(&child, command_line[0], &actions, NULL,
				  (char *const *)command_line, env);
	} else {
		rc = posix_spawn(&child, command_line[0], &actions, NULL,
				 (char *const *)command_line, env);
	}

	posix_spawn_file_actions_destroy(&actions);

	if (rc != 0) {
		close(stdinfd[0]);
		close(stdinfd[1]);
		close(stdoutfd[0]);
		close(stdoutfd[1]);
		close(stderrfd[0]);
		close(stderrfd[1]);
		return -1;
	}

	/* Parent: close child's ends, wrap ours in FILE* */
	close(stdinfd[0]);
	close(stdoutfd[1]);
	close(stderrfd[1]);

	out_process->stdin_file = fdopen(stdinfd[1], "wb");
	out_process->stdout_file = fdopen(stdoutfd[0], "rb");
	out_process->stderr_file = fdopen(stderrfd[0], "rb");
	out_process->child = child;
	out_process->return_status = 0;

	if (!out_process->stdin_file || !out_process->stdout_file) {
		/* Critical streams failed — clean up everything */
		if (out_process->stdin_file)
			fclose(out_process->stdin_file);
		else
			close(stdinfd[1]);
		if (out_process->stdout_file)
			fclose(out_process->stdout_file);
		else
			close(stdoutfd[0]);
		if (out_process->stderr_file)
			fclose(out_process->stderr_file);
		else
			close(stderrfd[0]);
		waitpid(child, NULL, 0);
		return -1;
	}

	return 0;
}

FILE *subprocess_stdin(const struct subprocess_s *const process) {
	return process->stdin_file;
}

FILE *subprocess_stdout(const struct subprocess_s *const process) {
	return process->stdout_file;
}

int subprocess_join(struct subprocess_s *const process,
		    int *const out_return_code) {
	int status;

	/* Close stdin so the child sees EOF */
	if (process->stdin_file) {
		fclose(process->stdin_file);
		process->stdin_file = NULL;
	}

	if (process->child) {
		if (process->child != waitpid(process->child, &status, 0))
			return -1;

		process->child = 0;

		if (WIFEXITED(status))
			process->return_status = WEXITSTATUS(status);
		else
			process->return_status = EXIT_FAILURE;
	}

	if (out_return_code)
		*out_return_code = process->return_status;

	return 0;
}

int subprocess_destroy(struct subprocess_s *const process) {
	if (process->stdin_file) {
		fclose(process->stdin_file);
		process->stdin_file = NULL;
	}
	if (process->stdout_file) {
		fclose(process->stdout_file);
		process->stdout_file = NULL;
	}
	if (process->stderr_file) {
		fclose(process->stderr_file);
		process->stderr_file = NULL;
	}
	return 0;
}

#else /* EMIL_DISABLE_SHELL */

/* Empty translation unit — ISO C forbids it, so provide a stub.
 * `typedef` is not a definition and satisfies pedantic compilers. */
typedef int emil_subprocess_unused_t;

#endif /* EMIL_DISABLE_SHELL */
