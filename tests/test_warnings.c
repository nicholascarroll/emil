/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_warnings.c: persistent status-bar warning state. */

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
	emil_strlcpy(tmpname, "/tmp/emil_warn_XXXXXX", sizeof(tmpname));
	int fd = mkstemp(tmpname);
	if (fd < 0)
		return NULL;
	if (content) {
		if (write(fd, content, strlen(content)) < 0) {
			close(fd);
			unlink(tmpname);
			return NULL;
		}
	}
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

/* Reset the throttle so the next checkFileModified runs immediately.
 * Without this, the 2-second throttle suppresses back-to-back calls
 * within the same test or across consecutive tests. */
static void resetThrottle(void) {
	resetFileCheckThrottle();
}

/* ---- external_mod / checkFileModified ---- */

void test_external_mod_not_set_before_change(void) {
	char *path = make_temp_file("hello\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));
	TEST_ASSERT_FALSE(buf->external_mod);

	/* refreshScreen would call checkFileModified; call it directly. */
	resetThrottle();
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
	resetThrottle();
	checkFileModified();
	TEST_ASSERT_TRUE(buf->external_mod);

	/* One-shot: once set, doesn't unset on its own. */
	resetThrottle();
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
	resetThrottle();
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
	int ready[2];	/* child → parent: lock placed */
	int release[2]; /* parent → child: please exit */
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
		if (write(ready[1], &ok, 1) != 1)
			_exit(3);
		close(ready[1]);
		char buf;
		if (read(release[0], &buf, 1) != 1)
			_exit(4);
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
	if (write(release_fd, &b, 1) != 1) {
		/* Best-effort signal to child; nothing to recover. */
	}
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

	/* editorOpen now probes the lock and sets lock_blocked_pid
	 * and read_only when another process holds the lock (#58). */
	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	TEST_ASSERT_EQUAL_INT((int)child, b->lock_blocked_pid);
	TEST_ASSERT_TRUE(b->read_only);
	TEST_ASSERT_EQUAL_INT(-1, b->lock_fd);

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
	TEST_ASSERT_TRUE(b->read_only);
	TEST_ASSERT(b->lock_blocked_pid != 0);

	/* Child releases the lock. */
	release_and_reap(child, release_fd, ready_fd);

	/* User toggles writable (C-x C-q), then edits — the fresh
	 * markBufferDirty triggers a lock acquire, which now succeeds. */
	b->read_only = 0;
	markBufferDirty(b);
	TEST_ASSERT_TRUE(b->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(0, b->lock_blocked_pid);

	unlink(path);
	free(path);
}

/* External_mod is a latch.  Once set, it must only be cleared by save
 * (user clobbers) or revert (user takes disk version). 
 */

void test_external_mod_persists_through_undo_to_clean(void) {
	char *path = make_temp_file("original\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));

	/* User dirties the buffer — acquires lock. */
	markBufferDirty(b);
	TEST_ASSERT_TRUE(b->dirty);
	TEST_ASSERT_TRUE(b->lock_fd >= 0);

	/* File changes on disk (external edit that ignored our lock). */
	bump_mtime(path, 2);

	/* Refresh notices the drift. */
	resetThrottle();
	checkFileModified();
	TEST_ASSERT_TRUE(b->external_mod);

	/* User undoes all their edits back to clean.  This calls
	 * markBufferClean → releaseLock.  external_mod must survive. */
	markBufferClean(b);
	TEST_ASSERT_FALSE(b->dirty);
	TEST_ASSERT_EQUAL_INT(-1, b->lock_fd);
	TEST_ASSERT_TRUE(b->external_mod); /* the flag is a latch */

	unlink(path);
	free(path);
}

/* Regression test for the "stale PID" defect: when the buffer is
 * dirty and another process held the lock, markBufferDirty
 * short-circuits on subsequent edits, so a manual clean/dirty cycle
 * as above is not a real-world recovery path.  checkFileModified
 * must re-probe the lock on every refresh while lock_blocked_pid
 * is set but lock_fd is not, and clear the warning when the holder
 * releases.  Without this the status bar shows a stale PID forever. */

void test_checkFileModified_reacquires_stale_lock(void) {
	char *path = make_temp_file("shared\n");
	TEST_ASSERT_NOT_NULL(path);

	int release_fd, ready_fd;
	pid_t child = fork_lock_holder(path, &release_fd, &ready_fd);
	TEST_ASSERT(child > 0);
	char rbuf;
	TEST_ASSERT_EQUAL_INT(1, read(ready_fd, &rbuf, 1));

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));

	/* User toggles writable (C-x C-q) before editing. */
	b->read_only = 0;

	/* User's first edit — lock acquisition fails, PID recorded. */
	markBufferDirty(b);
	TEST_ASSERT_TRUE(b->dirty);
	TEST_ASSERT_EQUAL_INT(-1, b->lock_fd);
	TEST_ASSERT_EQUAL_INT((int)child, b->lock_blocked_pid);

	/* User keeps typing.  markBufferDirty short-circuits, so the
	 * warning state is unchanged. */
	markBufferDirty(b);
	TEST_ASSERT_EQUAL_INT(-1, b->lock_fd);
	TEST_ASSERT_EQUAL_INT((int)child, b->lock_blocked_pid);

	/* Blocking process exits — lock is released at the OS level,
	 * but our buffer state hasn't observed that yet. */
	release_and_reap(child, release_fd, ready_fd);
	TEST_ASSERT_EQUAL_INT((int)child, b->lock_blocked_pid);

	/* refreshScreen tick: checkFileModified re-probes, acquires
	 * the now-available lock, and clears the warning. */
	resetThrottle();
	checkFileModified();
	TEST_ASSERT_TRUE(b->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(0, b->lock_blocked_pid);
	TEST_ASSERT_FALSE(b->external_mod); /* no on-disk change */

	unlink(path);
	free(path);
}

/* If the blocking process modified and saved the file before
 * releasing the lock, mtime drift fires external_mod first, which
 * deliberately suppresses the lock re-probe: acquiring the lock
 * now would let us silently clobber the other process's save.
 * lock_blocked_pid remains set but is shadowed in the status bar
 * by the higher-precedence [FILE CHANGED ON DISK] warning. */

void test_checkFileModified_does_not_reacquire_if_file_changed(void) {
	char *path = make_temp_file("shared\n");
	TEST_ASSERT_NOT_NULL(path);

	int release_fd, ready_fd;
	pid_t child = fork_lock_holder(path, &release_fd, &ready_fd);
	TEST_ASSERT(child > 0);
	char rbuf;
	TEST_ASSERT_EQUAL_INT(1, read(ready_fd, &rbuf, 1));

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	b->read_only = 0;
	markBufferDirty(b);
	TEST_ASSERT_EQUAL_INT((int)child, b->lock_blocked_pid);

	/* Simulate the blocking process saving before exiting. */
	release_and_reap(child, release_fd, ready_fd);
	bump_mtime(path, 2);

	resetThrottle();
	checkFileModified();
	TEST_ASSERT_TRUE(b->external_mod);
	TEST_ASSERT_EQUAL_INT(-1, b->lock_fd); /* did NOT acquire */
	TEST_ASSERT_EQUAL_INT((int)child, b->lock_blocked_pid); /* unchanged */

	unlink(path);
	free(path);
}

/* ---- save / revert clearing external_mod ---- */

/* save(0) clears external_mod after writing.  We can't easily call
 * save(0) in the test harness (it needs terminal I/O for prompts),
 * so we simulate the flag-clearing sequence that save(0) performs
 * after a successful write:
 *   markBufferClean → stat → open_mtime update → external_mod = 0
 * The point of this test is that the latch is cleared by the save
 * path but NOT by markBufferClean alone. */

void test_save_clears_external_mod(void) {
	char *path = make_temp_file("original\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));

	/* User dirties the buffer. */
	markBufferDirty(b);
	TEST_ASSERT_TRUE(b->dirty);

	/* File changes on disk. */
	bump_mtime(path, 2);
	resetThrottle();
	checkFileModified();
	TEST_ASSERT_TRUE(b->external_mod);

	/* Simulate save's post-write sequence. */
	markBufferClean(b);
	struct stat st;
	if (stat(path, &st) == 0)
		b->open_mtime = st.st_mtime;
	b->external_mod = 0;

	TEST_ASSERT_FALSE(b->external_mod);
	TEST_ASSERT_FALSE(b->dirty);

	/* Subsequent checkFileModified should not re-fire —
	 * open_mtime now matches the file. */
	resetThrottle();
	checkFileModified();
	TEST_ASSERT_FALSE(b->external_mod);

	unlink(path);
	free(path);
}

/* revert() replaces the buffer with a fresh editorOpen, which
 * initializes external_mod to 0.  Simulate by opening a new
 * buffer on the same path after external_mod was set. */

void test_revert_clears_external_mod(void) {
	char *path = make_temp_file("original\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));

	bump_mtime(path, 2);
	resetThrottle();
	checkFileModified();
	TEST_ASSERT_TRUE(b->external_mod);

	/* Simulate revert: open a fresh buffer on the same file. */
	destroyBuffer(b);
	struct buffer *fresh = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(fresh, path));
	TEST_ASSERT_FALSE(fresh->external_mod);
	TEST_ASSERT_FALSE(fresh->dirty);

	unlink(path);
	free(path);
}

/* ---- regressions ---- */

/* releaseLock used to zero open_mtime unconditionally.  markBufferClean
 * calls it on the dirty->clean edge, which a plain run of C-_ back to
 * the start of the session reaches -- and checkFileModified's job 1 is
 * gated on open_mtime != 0, so external-modification detection went
 * permanently dead for that buffer.  The lock lifetime and the mtime
 * baseline are unrelated and must not share a reset. */
void test_open_mtime_survives_clean_transition(void) {
	char *path = make_temp_file("original\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	TEST_ASSERT(b->open_mtime != 0);

	/* Edit, then undo all the way back to clean. */
	markBufferDirty(b);
	markBufferClean(b);
	TEST_ASSERT_FALSE(b->dirty);
	TEST_ASSERT_EQUAL_INT(-1, b->lock_fd);

	/* The baseline must still be there... */
	TEST_ASSERT(b->open_mtime != 0);

	/* ...and detection must still work. */
	bump_mtime(path, 2);
	resetThrottle();
	checkFileModified();
	TEST_ASSERT_TRUE(b->external_mod);

	unlink(path);
	free(path);
}

/* A buffer opened read-only BECAUSE another process held the lock can
 * never become dirty, so the old dirty-gated job 2 never ran for it and
 * the status bar displayed a PID that had long since exited for the
 * rest of the session.  The re-probe must run for a clean buffer too,
 * and clearing the lock must lift the read-only we imposed for it. */
void test_readonly_lifted_when_lock_released(void) {
	char *path = make_temp_file("shared\n");
	TEST_ASSERT_NOT_NULL(path);

	int release_fd, ready_fd;
	pid_t child = fork_lock_holder(path, &release_fd, &ready_fd);
	TEST_ASSERT(child > 0);
	char rbuf;
	TEST_ASSERT_EQUAL_INT(1, read(ready_fd, &rbuf, 1));

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	TEST_ASSERT_TRUE(b->read_only);
	TEST_ASSERT_TRUE(b->read_only_by_lock);
	TEST_ASSERT_EQUAL_INT((int)child, b->lock_blocked_pid);

	/* Holder exits.  The buffer is clean, so the check probes
	 * rather than acquiring: the lock is held only while there
	 * are unsaved changes. */
	release_and_reap(child, release_fd, ready_fd);
	resetThrottle();
	checkFileModified();

	TEST_ASSERT_EQUAL_INT(0, b->lock_blocked_pid);
	TEST_ASSERT_FALSE(b->read_only);
	TEST_ASSERT_FALSE(b->read_only_by_lock);
	/* Probed, not acquired. */
	TEST_ASSERT_EQUAL_INT(-1, b->lock_fd);

	unlink(path);
	free(path);
}

/* A read-only imposed by anything other than the lock is not ours to
 * undo.  Here the user asked for it explicitly (C-x C-q / find-file
 * read-only), so releasing the lock must leave it alone. */
void test_user_readonly_not_lifted_by_lock_release(void) {
	char *path = make_temp_file("shared\n");
	TEST_ASSERT_NOT_NULL(path);

	int release_fd, ready_fd;
	pid_t child = fork_lock_holder(path, &release_fd, &ready_fd);
	TEST_ASSERT(child > 0);
	char rbuf;
	TEST_ASSERT_EQUAL_INT(1, read(ready_fd, &rbuf, 1));

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	TEST_ASSERT_TRUE(b->read_only_by_lock);

	/* User makes the read-only their own choice. */
	b->read_only_by_lock = 0;

	release_and_reap(child, release_fd, ready_fd);
	resetThrottle();
	checkFileModified();

	TEST_ASSERT_EQUAL_INT(0, b->lock_blocked_pid);
	TEST_ASSERT_TRUE(b->read_only); /* still the user's choice */

	unlink(path);
	free(path);
}

/* checkFileModified must not run against a special buffer.  Its name
 * is non-NULL ("*scratch*", "*stdin*", "*Shell Output*"), so the
 * filename guard alone let it through to stat() a literal "*stdin*"
 * in the cwd every two seconds. */
void test_special_buffer_is_not_checked(void) {
	struct buffer *b = make_test_buffer("output\n");
	b->filename = xstrdup("*stdin*");
	b->special_buffer = 1;
	b->open_mtime = 1;
	b->lock_blocked_pid = 4242;

	resetThrottle();
	checkFileModified();

	TEST_ASSERT_FALSE(b->external_mod);
	TEST_ASSERT_EQUAL_INT(4242, b->lock_blocked_pid);
}

/* ---- pre-save confirmation ---- */

/* Read the file back so the test can tell whether the write landed. */
static char *slurp(const char *path) {
	FILE *fp = fopen(path, "r");
	if (!fp)
		return NULL;
	static char buf[512];
	size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	fclose(fp);
	return buf;
}

/* The authoritative check: an external write that the background poll
 * never saw must still be caught at save time, and answering "n" must
 * leave the file alone. */
void test_presave_prompt_refused_leaves_file(void) {
	char *path = make_temp_file("original\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	markBufferDirty(b);

	/* Someone else rewrites the file.  No poll runs, so the status
	 * bar never lit: external_mod is still clear. */
	FILE *fp = fopen(path, "w");
	TEST_ASSERT_NOT_NULL(fp);
	fputs("theirs\n", fp);
	fclose(fp);
	bump_mtime(path, 2);
	TEST_ASSERT_FALSE(b->external_mod);

	int keys[] = { 'n' };
	scriptKeys(keys, 1);
	muteStdout();
	save(0);
	unmuteStdout();
	clearKeys();

	/* Refused: their content survives, ours stays unsaved. */
	TEST_ASSERT_EQUAL_STRING("theirs\n", slurp(path));
	TEST_ASSERT_TRUE(b->dirty);
	/* ...and the indicator is now lit, so the next save won't ask. */
	TEST_ASSERT_TRUE(b->external_mod);

	unlink(path);
	free(path);
}

/* Answering "y" goes through and clobbers, as asked. */
void test_presave_prompt_accepted_writes(void) {
	char *path = make_temp_file("original\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	b->cx = 0;
	b->cy = 0;
	insertRow(b, 0, (const uint8_t *)"ours", 4);
	markBufferDirty(b);

	FILE *fp = fopen(path, "w");
	TEST_ASSERT_NOT_NULL(fp);
	fputs("theirs\n", fp);
	fclose(fp);
	bump_mtime(path, 2);

	int keys[] = { 'y' };
	scriptKeys(keys, 1);
	muteStdout();
	save(0);
	unmuteStdout();
	clearKeys();

	TEST_ASSERT(strstr(slurp(path), "ours") != NULL);
	TEST_ASSERT_FALSE(b->dirty);

	unlink(path);
	free(path);
}


/* An unmodified file is saved with no prompt at all. */
void test_presave_no_prompt_when_unchanged(void) {
	char *path = make_temp_file("original\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *b = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(b, path));
	markBufferDirty(b);

	clearKeys();
	muteStdout();
	save(0);
	unmuteStdout();

	TEST_ASSERT_FALSE(b->dirty);
	TEST_ASSERT_FALSE(b->external_mod);

	unlink(path);
	free(path);
}

/* ---- §F2: a second open+close of an already-locked file ---------- */

/* Does *some* process hold an advisory write lock on `path`?
 *
 * Must be asked from another process.  POSIX F_GETLK reports F_UNLCK
 * for a lock the calling process holds itself, so emil cannot see its
 * own lock and a same-process probe would answer "no" even while the
 * lock is held.  That is also why the defect below is silent: nothing
 * inside emil can notice the lock has gone. */
static int lock_held_on(const char *path) {
	pid_t pid = fork();
	if (pid < 0)
		return -1;
	if (pid == 0) {
		int fd = open(path, O_RDONLY);
		if (fd < 0)
			_exit(2);
		struct flock q;
		memset(&q, 0, sizeof(q));
		q.l_type = F_WRLCK;
		q.l_whence = SEEK_SET;
		q.l_start = 0;
		q.l_len = 0;
		int held = (fcntl(fd, F_GETLK, &q) == 0 && q.l_type != F_UNLCK);
		close(fd);
		_exit(held ? 1 : 0);
	}
	int st = 0;
	if (waitpid(pid, &st, 0) != pid || !WIFEXITED(st))
		return -1;
	return WEXITSTATUS(st) == 1;
}

/* Are two names for one file one lockable object on this platform?
 *
 * The three symlink tests below all rest on the POSIX ownership rule:
 * a record lock belongs to (process, inode), so a lock taken through
 * one name is reported on the other, and closing any descriptor on
 * the inode drops every lock the process holds on it.  That rule is
 * what makes the sibling defect possible and its repair observable.
 *
 * It does not hold everywhere, and the failure is silent.  On MSYS2
 * (Cygwin's emulation of POSIX record locking) CI failed
 * test_release_of_one_lock_keeps_same_inode_siblings on exactly the
 * one assertion that needs cross-name identity, while every
 * single-name assertion in this file passed: the same-path close
 * still dropped the lock, and relockAll()'s re-assert on the retained
 * descriptor was still visible to an outside prober.  Only the lock
 * taken through the symlink was not reported on the target.  So the
 * two descriptors are not one lockable object there.  That reading is
 * inferred from the CI log; no MSYS2 host was available to reproduce
 * it, and which half of the emulation causes it -- lock records keyed
 * by something other than the inode, or a symlink that is not one --
 * is not established.
 *
 * Either way the question is answerable at runtime instead of assumed,
 * and answering it is what this does: lock a scratch file through a
 * symlink to it and ask another process whether the lock is visible on
 * the target.  Where it is not, the defect these tests describe cannot
 * arise and its repair cannot be observed, so they skip rather than
 * pass while asserting nothing. */
static int symlink_names_share_locks(void) {
	char *target = make_temp_file("x\n");
	if (target == NULL)
		return 0;

	char link[80];
	emil_strlcpy(link, "/tmp/emil_warn_pre_XXXXXX", sizeof(link));
	int lfd = mkstemp(link);
	if (lfd < 0) {
		unlink(target);
		free(target);
		return 0;
	}
	close(lfd);
	unlink(link);

	int shared = 0;
	if (symlink(target, link) == 0) {
		int fd = open(link, O_RDWR);
		if (fd >= 0) {
			struct flock fl;
			memset(&fl, 0, sizeof(fl));
			fl.l_type = F_WRLCK;
			fl.l_whence = SEEK_SET;
			fl.l_start = 0;
			fl.l_len = 0;
			if (fcntl(fd, F_SETLK, &fl) == 0)
				shared = (lock_held_on(target) == 1);
			close(fd);
		}
		unlink(link);
	}

	unlink(target);
	free(target);
	return shared;
}

/* C-x i on the file the buffer is already visiting.
 *
 * insertFileAtPath fopen()s the path and fclose()s it.  POSIX drops
 * every lock a process holds on an inode when it closes *any*
 * descriptor referring to it, so that fclose silently released the
 * lock taken by markBufferDirty -- while lock_fd stayed open, so emil
 * went on believing it held one.  The two assertions at the end are
 * the whole defect: emil's belief, and the truth. */
void test_insert_file_keeps_our_lock(void) {
	char *path = make_temp_file("content\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));
	markBufferDirty(buf);
	TEST_ASSERT_TRUE(buf->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path)); /* baseline */

	TEST_ASSERT_EQUAL_INT(0, insertFileAtPath(buf, path, path));

	TEST_ASSERT_TRUE(buf->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path));

	unlink(path);
	free(path);
}

/* C-x C-f on a symlink to the open, dirty file.
 *
 * Same mechanism through a different door: editorOpen on a second
 * buffer opens and closes the same inode.  The symlink is what stops
 * the find-file path recognising it as a file already open, but the
 * inode is what the kernel cares about, so the first buffer's lock
 * goes.  The second buffer is chained onto E.headbuf because the fix
 * walks the buffer list. */
void test_open_second_buffer_keeps_first_buffers_lock(void) {
	if (!symlink_names_share_locks()) {
		TEST_SKIP("a lock taken through a symlink is not reported "
			  "on its target here");
		return;
	}
	char *path = make_temp_file("content\n");
	TEST_ASSERT_NOT_NULL(path);
	/* Named via mkstemp rather than built from `path` with a "%s"
	 * format: gcc cannot see that TEST_ASSERT_NOT_NULL returned, so
	 * the latter draws -Wformat-truncation on the sanitizer build. */
	char linkpath[80];
	emil_strlcpy(linkpath, "/tmp/emil_warn_lnk_XXXXXX", sizeof(linkpath));
	int lfd = mkstemp(linkpath);
	TEST_ASSERT_TRUE(lfd >= 0);
	close(lfd);
	unlink(linkpath);
	TEST_ASSERT_EQUAL_INT(0, symlink(path, linkpath));

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));
	markBufferDirty(buf);
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path));

	struct buffer *other = newBuffer();
	buf->next = other;
	TEST_ASSERT_EQUAL_INT(0, editorOpen(other, linkpath));

	TEST_ASSERT_TRUE(buf->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path));

	unlink(linkpath);
	unlink(path);
	free(path);
}

/* The window between the close and the re-assert is real, and this is
 * what happens when a rival wins it.
 *
 * relockAll() must not pretend: it has to leave lock_fd < 0 (emil holds
 * nothing) and name the holder in lock_blocked_pid, which is what
 * routes into the existing LOCK_CONFLICT presentation and re-arms the
 * background re-probe -- that retry is gated on
 * lock_blocked_pid != 0 && lock_fd < 0, so a lock_fd left dangling
 * would also stop emil ever noticing the rival went away.
 *
 * The drop is staged by hand rather than by calling insertFileAtPath,
 * because the rival has to take the lock inside a window that a real
 * fopen/fclose pair closes far too fast to hit reliably. */
void test_relock_reports_conflict_when_rival_takes_the_lock(void) {
	char *path = make_temp_file("content\n");
	TEST_ASSERT_NOT_NULL(path);

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));
	markBufferDirty(buf);
	TEST_ASSERT_TRUE(buf->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path));

	/* Exactly what an unrelated fclose() does to us. */
	int scratch = open(path, O_RDONLY);
	TEST_ASSERT_TRUE(scratch >= 0);
	close(scratch);
	TEST_ASSERT_EQUAL_INT(0, lock_held_on(path));

	int release_fd, ready_fd;
	pid_t child = fork_lock_holder(path, &release_fd, &ready_fd);
	TEST_ASSERT(child > 0);
	char ready;
	TEST_ASSERT_EQUAL_INT(1, (int)read(ready_fd, &ready, 1));

	relockAll();

	TEST_ASSERT_EQUAL_INT(-1, buf->lock_fd);
	TEST_ASSERT_EQUAL_INT((int)child, buf->lock_blocked_pid);

	release_and_reap(child, release_fd, ready_fd);
	unlink(path);
	free(path);
}

/* The background poll opens and closes the file too.
 *
 * checkFileModified's stale-lock job calls probeLock(), which is one
 * open() and one close() on the buffer's inode -- the same pair that
 * dropped the lock in the two tests above, arriving on a timer instead
 * of a keystroke.  Reachable when the focused buffer carries a stale
 * lock warning while another buffer holds a lock on the same inode
 * through a symlink.
 *
 * The stale warning is staged by hand rather than with
 * fork_lock_holder, because it cannot be manufactured honestly here: a
 * rival cannot take the lock while `buf` holds it, and the state being
 * simulated -- a holder that has exited since the warning was set -- is
 * ordinary, since unfocused buffers never poll and their warnings go
 * stale as a matter of course. */
void test_lock_poll_probe_keeps_other_buffers_lock(void) {
	if (!symlink_names_share_locks()) {
		TEST_SKIP("a lock taken through a symlink is not reported "
			  "on its target here");
		return;
	}
	char *path = make_temp_file("content\n");
	TEST_ASSERT_NOT_NULL(path);
	char linkpath[80];
	emil_strlcpy(linkpath, "/tmp/emil_warn_lnk_XXXXXX", sizeof(linkpath));
	int lfd = mkstemp(linkpath);
	TEST_ASSERT_TRUE(lfd >= 0);
	close(lfd);
	unlink(linkpath);
	TEST_ASSERT_EQUAL_INT(0, symlink(path, linkpath));

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));
	markBufferDirty(buf);
	TEST_ASSERT_TRUE(buf->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path));

	struct buffer *other = newBuffer();
	buf->next = other;
	TEST_ASSERT_EQUAL_INT(0, editorOpen(other, linkpath));
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path)); /* survived the open */

	/* A departed rival's warning, not yet re-probed. */
	other->lock_blocked_pid = 999999;
	other->lock_fd = -1;
	other->external_mod = 0;
	TEST_ASSERT_FALSE(other->dirty); /* so the poll probes, not locks */

	E.buf = other;
	resetThrottle();
	checkFileModified();
	E.buf = buf;

	TEST_ASSERT_TRUE(buf->lock_fd >= 0); /* emil's belief... */
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path)); /* ...and the truth */

	unlink(linkpath);
	unlink(path);
	free(path);
}

/* Releasing one buffer's lock must not release its sibling's.
 *
 * Two dirty buffers on one inode (again through a symlink) each hold a
 * lock_fd, but the kernel holds one per-process lock record between
 * them.  releaseLock() on either buffer close()s a descriptor on the
 * inode, which drops that shared record -- so saving or killing one
 * buffer silently unlocked the other, which is still dirty and still
 * believes it is protected.  markBufferClean() is the save-path door
 * to it, exercised here; destroyBuffer() is the other. */
void test_release_of_one_lock_keeps_same_inode_siblings(void) {
	if (!symlink_names_share_locks()) {
		TEST_SKIP("a lock taken through a symlink is not reported "
			  "on its target here");
		return;
	}
	char *path = make_temp_file("content\n");
	TEST_ASSERT_NOT_NULL(path);
	char linkpath[80];
	emil_strlcpy(linkpath, "/tmp/emil_warn_lnk_XXXXXX", sizeof(linkpath));
	int lfd = mkstemp(linkpath);
	TEST_ASSERT_TRUE(lfd >= 0);
	close(lfd);
	unlink(linkpath);
	TEST_ASSERT_EQUAL_INT(0, symlink(path, linkpath));

	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, path));
	markBufferDirty(buf);
	TEST_ASSERT_TRUE(buf->lock_fd >= 0);

	struct buffer *other = newBuffer();
	buf->next = other;
	TEST_ASSERT_EQUAL_INT(0, editorOpen(other, linkpath));
	markBufferDirty(other);
	/* Same process, so no conflict: both buffers now hold fds. */
	TEST_ASSERT_TRUE(other->lock_fd >= 0);
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path));

	markBufferClean(buf); /* the save path's release */

	TEST_ASSERT_TRUE(other->lock_fd >= 0); /* still dirty, still believes */
	TEST_ASSERT_EQUAL_INT(1, lock_held_on(path));

	unlink(linkpath);
	unlink(path);
	free(path);
}

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
	RUN_TEST(test_external_mod_persists_through_undo_to_clean);
	RUN_TEST(test_checkFileModified_reacquires_stale_lock);
	RUN_TEST(test_checkFileModified_does_not_reacquire_if_file_changed);

	RUN_TEST(test_open_mtime_survives_clean_transition);
	RUN_TEST(test_readonly_lifted_when_lock_released);
	RUN_TEST(test_user_readonly_not_lifted_by_lock_release);
	RUN_TEST(test_special_buffer_is_not_checked);

	RUN_TEST(test_save_clears_external_mod);
	RUN_TEST(test_revert_clears_external_mod);

	RUN_TEST(test_presave_prompt_refused_leaves_file);
	RUN_TEST(test_presave_prompt_accepted_writes);
	RUN_TEST(test_presave_no_prompt_when_unchanged);

	RUN_TEST(test_insert_file_keeps_our_lock);
	RUN_TEST(test_open_second_buffer_keeps_first_buffers_lock);
	RUN_TEST(test_relock_reports_conflict_when_rival_takes_the_lock);
	RUN_TEST(test_lock_poll_probe_keeps_other_buffers_lock);
	RUN_TEST(test_release_of_one_lock_keeps_same_inode_siblings);

	return TEST_END();
}
