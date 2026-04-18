/* test_warnings.c — persistent status-bar warning state.
 *
 * Covers the three persistent warnings that live in the status bar's
 * right-hand block:
 *   - E.memory_over_limit       (set by recheckMemoryBudget)
 *   - buf->external_mod         (set by checkFileModified)
 *   - buf->lock_blocked_pid     (set by lockFile via markBufferDirty)
 *
 * The messages themselves are rendered by display.c, which isn't
 * exercised here (drawStatusBar writes to an abuf).  These tests
 * cover the state transitions: when does each flag set, when does
 * it clear, and what happens when multiple conditions interact —
 * particularly the rule that a buffer with external_mod set must
 * not acquire a lock on subsequent edits (would silently clobber
 * the other process's work on save).
 */

#include "test.h"
#include "test_harness.h"
#include "buffer.h"
#include "fileio.h"
#include "region.h"
#include "util.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <time.h>

/* ---- helpers ---- */

/* Write `content` to a fresh temp file; return mallocd path. */
static char *make_temp_file(const char *content) {
	static char tmpname[64];
	strcpy(tmpname, "/tmp/emil_warn_XXXXXX");
	int fd = mkstemp(tmpname);
	if (fd < 0)
		return NULL;
	if (content)
		write(fd, content, strlen(content));
	close(fd);
	return strdup(tmpname);
}

/* Bump a file's mtime by `delta` seconds so checkFileModified fires. */
static void bump_mtime(const char *path, int delta) {
	struct stat st;
	if (stat(path, &st) != 0)
		return;
	struct timespec times[2];
	times[0].tv_sec = st.st_atime;
	times[0].tv_nsec = 0;
	times[1].tv_sec = st.st_mtime + delta;
	times[1].tv_nsec = 0;
	utimensat(AT_FDCWD, path, times, 0);
}

/* ---- external_mod / checkFileModified ---- */

void test_external_mod_not_set_before_change(void) {
	char *path = make_temp_file("hello\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));
	TEST_ASSERT_FALSE(buf->external_mod);

	/* refreshScreen would call checkFileModified; call it directly. */
	checkFileModified();
	TEST_ASSERT_FALSE(buf->external_mod);

	unlink(path);
	free(path);
}

void test_external_mod_set_on_mtime_change(void) {
	char *path = make_temp_file("original\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));

	/* Simulate another process touching the file. */
	bump_mtime(path, 10);
	checkFileModified();
	TEST_ASSERT_TRUE(buf->external_mod);

	/* One-shot: once set, doesn't unset on its own. */
	checkFileModified();
	TEST_ASSERT_TRUE(buf->external_mod);

	unlink(path);
	free(path);
}

/* ---- the "no lock when external_mod set" rule ---- */

void test_markdirty_skips_lock_when_externally_modified(void) {
	char *path = make_temp_file("content\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));
	TEST_ASSERT_EQUAL_INT(-1, buf->lock_fd); /* "lock only while dirty" */

	/* External process modifies the file; flag lights up. */
	bump_mtime(path, 10);
	checkFileModified();
	TEST_ASSERT_TRUE(buf->external_mod);

	/* User now edits.  Without the guard, markBufferDirty would grab
	 * the lock — but the buffer no longer reflects disk, so saving
	 * would silently clobber the other process's work.  The guard
	 * must prevent lock acquisition here. */
	markBufferDirty(buf);
	TEST_ASSERT_TRUE(buf->dirty);
	TEST_ASSERT_EQUAL_INT(-1, buf->lock_fd); /* no lock acquired */

	unlink(path);
	free(path);
}

void test_markdirty_normal_path_still_locks(void) {
	/* Sanity check: when external_mod is NOT set, the normal path
	 * does acquire the lock.  Otherwise the guard above could hide
	 * a regression that disabled all locking. */
	char *path = make_temp_file("content\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));
	TEST_ASSERT_FALSE(buf->external_mod);
	TEST_ASSERT_EQUAL_INT(-1, buf->lock_fd);

	markBufferDirty(buf);
	TEST_ASSERT_TRUE(buf->dirty);
	TEST_ASSERT_TRUE(buf->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(0, buf->lock_blocked_pid);

	unlink(path);
	free(path);
}

/* ---- lock_blocked_pid state machine ----
 *
 * fcntl advisory locks are per-process: a second fd opened by the
 * same process gets its own lock with no conflict.  To simulate a
 * different process holding the lock, we fork a child that takes
 * the lock, signals readiness via a pipe, and waits for us to
 * signal release.  The parent runs the assertions. */

/* Fork a child that acquires a write lock on `path` and blocks on
 * reading from `release_fd`.  On success returns the child PID and
 * sets *release_fd to the write end of a pipe; writing any byte to
 * it lets the child exit (releasing the lock).  Returns -1 on error.
 *
 * The child writes one byte to the caller before blocking, so the
 * caller knows the lock is placed.  *ready_fd is the read end of
 * that pipe; caller should read one byte then close it. */
static pid_t fork_lock_holder(const char *path, int *release_write_fd,
			      int *ready_read_fd) {
	int ready[2];	 /* child → parent: lock placed */
	int release[2];	 /* parent → child: please exit */
	if (pipe(ready) != 0)
		return -1;
	if (pipe(release) != 0) {
		close(ready[0]);
		close(ready[1]);
		return -1;
	}

	pid_t pid = fork();
	if (pid < 0) {
		close(ready[0]);
		close(ready[1]);
		close(release[0]);
		close(release[1]);
		return -1;
	}
	if (pid == 0) {
		/* child */
		close(ready[0]);
		close(release[1]);
		int fd = open(path, O_RDWR);
		if (fd < 0)
			_exit(1);
		struct flock fl;
		memset(&fl, 0, sizeof(fl));
		fl.l_type = F_WRLCK;
		fl.l_whence = SEEK_SET;
		if (fcntl(fd, F_SETLK, &fl) != 0)
			_exit(2);
		/* Lock placed — signal parent and block. */
		char ok = 'R';
		write(ready[1], &ok, 1);
		close(ready[1]);
		char buf;
		read(release[0], &buf, 1); /* blocks until parent writes */
		close(release[0]);
		close(fd);
		_exit(0);
	}
	/* parent */
	close(ready[1]);
	close(release[0]);
	*release_write_fd = release[1];
	*ready_read_fd = ready[0];
	return pid;
}

static void release_and_reap(pid_t pid, int release_fd, int ready_fd) {
	char b = 'G';
	write(release_fd, &b, 1);
	close(release_fd);
	close(ready_fd);
	int status;
	waitpid(pid, &status, 0);
}

void test_lock_blocked_set_when_other_process_holds_lock(void) {
	char *path = make_temp_file("shared\n");
	TEST_ASSERT_NOT_NULL(path);

	int release_fd, ready_fd;
	pid_t child = fork_lock_holder(path, &release_fd, &ready_fd);
	TEST_ASSERT(child > 0);

	/* Wait for the child to confirm the lock is placed. */
	char buf;
	TEST_ASSERT_EQUAL_INT(1, read(ready_fd, &buf, 1));

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	TEST_ASSERT_EQUAL_INT(0, b->lock_blocked_pid);

	/* First edit attempt — lockFile fails, F_GETLK reveals the
	 * child PID, lock_blocked_pid is set to that PID. */
	markBufferDirty(b);
	TEST_ASSERT_TRUE(b->dirty);
	TEST_ASSERT_EQUAL_INT(-1, b->lock_fd);
	TEST_ASSERT_EQUAL_INT((int)child, b->lock_blocked_pid);

	release_and_reap(child, release_fd, ready_fd);
	unlink(path);
	free(path);
}

void test_lock_blocked_cleared_on_successful_acquire(void) {
	char *path = make_temp_file("shared\n");
	TEST_ASSERT_NOT_NULL(path);

	int release_fd, ready_fd;
	pid_t child = fork_lock_holder(path, &release_fd, &ready_fd);
	TEST_ASSERT(child > 0);
	char rbuf;
	TEST_ASSERT_EQUAL_INT(1, read(ready_fd, &rbuf, 1));

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	markBufferDirty(b);
	TEST_ASSERT(b->lock_blocked_pid != 0);

	/* Child releases the lock. */
	release_and_reap(child, release_fd, ready_fd);

	/* Buffer is still dirty; markBufferDirty short-circuits.
	 * Simulate a save (clean transition) then a fresh edit — that
	 * triggers a new acquire attempt, which now succeeds. */
	markBufferClean(b);
	markBufferDirty(b);
	TEST_ASSERT_TRUE(b->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(0, b->lock_blocked_pid);

	unlink(path);
	free(path);
}

/* ---- memory_over_limit ---- */

void test_memory_flag_initially_clear(void) {
	E.memory_over_limit = 0;
	recheckMemoryBudget();
	TEST_ASSERT_FALSE(E.memory_over_limit);
}

void test_memory_flag_clears_when_budget_under_limit(void) {
	/* Simulate the flag being latched from a prior over-limit event,
	 * then verify recheck clears it once the budget is back under. */
	struct buffer *buf = make_test_buffer("small");
	(void)buf;

	E.memory_over_limit = 1; /* stale flag */
	recheckMemoryBudget();
	TEST_ASSERT_FALSE(E.memory_over_limit);
}

/* Note: we don't test the "set on over-limit" side here — it would
 * require either allocating >1GB of buffer text (default
 * EMIL_MAX_OPEN_BYTES) or compiling with a reduced limit.  The
 * logic is a single comparison in recheckMemoryBudget; if the
 * "clears" test passes, the flag path itself works. */

/* ---- setUp / tearDown / main ---- */

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_external_mod_not_set_before_change);
	RUN_TEST(test_external_mod_set_on_mtime_change);

	RUN_TEST(test_markdirty_skips_lock_when_externally_modified);
	RUN_TEST(test_markdirty_normal_path_still_locks);

	RUN_TEST(test_lock_blocked_set_when_other_process_holds_lock);
	RUN_TEST(test_lock_blocked_cleared_on_successful_acquire);

	RUN_TEST(test_memory_flag_initially_clear);
	RUN_TEST(test_memory_flag_clears_when_budget_under_limit);

	return TEST_END();
}
