/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */

/* Fault injection for the backup half of the save path (#125).
 *
 * backup.c contains no test hooks and no test-only macros.  Instead
 * run_tests.sh copies it to tests/backup_faked.c and rewrites the
 * syscall names to the fk_* fakes defined below, so this suite drives
 * the real control flow against a filesystem that fails on command.
 * The generated copy is #included, not linked, so the copy's statics
 * are reachable and its one external symbol is renamed and cannot
 * collide with the real backup.o in TEST_OBJECTS.
 *
 * What the fakes model: a flat table of named files with contents, an
 * fd table, and a per-syscall fault schedule.  Enough to reach every
 * error branch; not a filesystem.
 */

#include "test.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*** fake filesystem ***/

#define FAKE_MAX_FILES 40
#define FAKE_MAX_FDS 16
#define FAKE_MAX_DATA 4096

struct fakeFile {
	int used;
	int is_dir;
	char name[PATH_MAX];
	char data[FAKE_MAX_DATA];
	size_t len;
};

struct fakeFd {
	int open_;
	int file; /* index into files[] */
	size_t pos;
};

static struct fakeFile files[FAKE_MAX_FILES];
static struct fakeFd fds[FAKE_MAX_FDS];

/* Fault schedule: fail the Nth call to a given syscall with errno E.
 * n_fail == 0 disables.  Counting starts at 1. */
struct faultSpec {
	int n_fail;
	int err;
	int count;
};

static struct faultSpec f_open, f_read, f_write, f_fsync, f_close, f_unlink;

/* Observability: what the unit did, for assertions the return value
 * alone cannot make. */
static int n_unlink_calls;
static char last_unlinked[PATH_MAX];

static void fakeReset(void) {
	memset(files, 0, sizeof(files));
	memset(fds, 0, sizeof(fds));
	memset(&f_open, 0, sizeof(f_open));
	memset(&f_read, 0, sizeof(f_read));
	memset(&f_write, 0, sizeof(f_write));
	memset(&f_fsync, 0, sizeof(f_fsync));
	memset(&f_close, 0, sizeof(f_close));
	memset(&f_unlink, 0, sizeof(f_unlink));
	n_unlink_calls = 0;
	last_unlinked[0] = '\0';
}

/* Returns 1 if this call should fail, setting errno. */
static int faultDue(struct faultSpec *f) {
	if (f->n_fail == 0)
		return 0;
	f->count++;
	if (f->count != f->n_fail)
		return 0;
	errno = f->err;
	return 1;
}

static int fileFind(const char *name) {
	for (int i = 0; i < FAKE_MAX_FILES; i++)
		if (files[i].used && strcmp(files[i].name, name) == 0)
			return i;
	return -1;
}

static int fileCreate(const char *name, int is_dir) {
	for (int i = 0; i < FAKE_MAX_FILES; i++) {
		if (files[i].used)
			continue;
		files[i].used = 1;
		files[i].is_dir = is_dir;
		files[i].len = 0;
		files[i].data[0] = '\0';
		snprintf(files[i].name, sizeof(files[i].name), "%s", name);
		return i;
	}
	return -1;
}

/* Seed a regular file with contents. */
static void fakePut(const char *name, const char *contents) {
	int i = fileFind(name);
	if (i < 0)
		i = fileCreate(name, 0);
	files[i].len = strlen(contents);
	memcpy(files[i].data, contents, files[i].len);
}

static void fakeMkdir(const char *name) {
	if (fileFind(name) < 0)
		(void)fileCreate(name, 1);
}

static int fakeExists(const char *name) {
	return fileFind(name) >= 0;
}

static const char *fakeContents(const char *name) {
	int i = fileFind(name);
	return i < 0 ? NULL : files[i].data;
}

/* Count file descriptors still open — the leak check. */
static int fakeOpenFds(void) {
	int n = 0;
	for (int i = 0; i < FAKE_MAX_FDS; i++)
		if (fds[i].open_)
			n++;
	return n;
}

/*** the fakes themselves ***/

static int fk_open(const char *name, int flags, ...) {
	if (faultDue(&f_open))
		return -1;

	int idx = fileFind(name);

	if (idx >= 0 && (flags & O_CREAT) && (flags & O_EXCL)) {
		errno = EEXIST;
		return -1;
	}
	if (idx < 0) {
		if (!(flags & O_CREAT)) {
			errno = ENOENT;
			return -1;
		}
		idx = fileCreate(name, 0);
		if (idx < 0) {
			errno = ENFILE;
			return -1;
		}
	}

	for (int i = 3; i < FAKE_MAX_FDS; i++) {
		if (fds[i].open_)
			continue;
		fds[i].open_ = 1;
		fds[i].file = idx;
		fds[i].pos = 0;
		return i;
	}
	errno = EMFILE;
	return -1;
}

static int fk_close(int fd) {
	if (fd < 0 || fd >= FAKE_MAX_FDS || !fds[fd].open_) {
		errno = EBADF;
		return -1;
	}
	/* A failing close still releases the descriptor, as on Linux. */
	fds[fd].open_ = 0;
	if (faultDue(&f_close))
		return -1;
	return 0;
}

static long fk_read(int fd, void *buf, unsigned long n) {
	if (faultDue(&f_read))
		return -1;
	if (fd < 0 || fd >= FAKE_MAX_FDS || !fds[fd].open_) {
		errno = EBADF;
		return -1;
	}
	struct fakeFile *f = &files[fds[fd].file];
	size_t pos = fds[fd].pos;
	/* The >= guard is what keeps `len - pos` from wrapping, and it
	 * has to be visible to the compiler, not merely true: without
	 * it -Warray-bounds reports a negative memcpy offset. */
	if (pos >= f->len || f->len > FAKE_MAX_DATA)
		return 0;
	size_t left = f->len - pos;
	if (n > left)
		n = left;
	memcpy(buf, f->data + pos, n);
	fds[fd].pos = pos + n;
	return (long)n;
}

static long fk_write(int fd, const void *buf, unsigned long n) {
	if (faultDue(&f_write))
		return -1;
	if (fd < 0 || fd >= FAKE_MAX_FDS || !fds[fd].open_) {
		errno = EBADF;
		return -1;
	}
	struct fakeFile *f = &files[fds[fd].file];
	size_t pos = fds[fd].pos;
	if (pos > FAKE_MAX_DATA || n > FAKE_MAX_DATA - pos) {
		errno = ENOSPC;
		return -1;
	}
	memcpy(f->data + pos, buf, n);
	fds[fd].pos = pos + n;
	if (fds[fd].pos > f->len)
		f->len = fds[fd].pos;
	return (long)n;
}

static int fk_fsync(int fd) {
	if (faultDue(&f_fsync))
		return -1;
	if (fd < 0 || fd >= FAKE_MAX_FDS || !fds[fd].open_) {
		errno = EBADF;
		return -1;
	}
	return 0;
}

static int fk_unlink(const char *name) {
	n_unlink_calls++;
	snprintf(last_unlinked, sizeof(last_unlinked), "%s", name);
	if (faultDue(&f_unlink))
		return -1;
	int i = fileFind(name);
	if (i < 0) {
		errno = ENOENT;
		return -1;
	}
	files[i].used = 0;
	return 0;
}

static int fk_fcntl(int fd, int cmd, ...) {
	(void)fd;
	(void)cmd;
	return 0;
}

/* The unit under test, with its syscalls redirected by run_tests.sh.
 * Declared before inclusion so -Wmissing-prototypes stays quiet. */
int fake_makeVerifiedBackup(const char *path, char *backup_path,
			    size_t backup_path_size);
#include "backup_faked.c"

/*** harness hooks ***/

void setUp(void) {
	fakeReset();
}

void tearDown(void) {
}

/*** helpers ***/

#define TARGET "/w/notes.txt"
#define BODY "hello backup\n"

static int runBackup(char *out, size_t out_size) {
	out[0] = '\0';
	return fake_makeVerifiedBackup(TARGET, out, out_size);
}

static void seedTarget(void) {
	fakeReset();
	fakeMkdir("/w");
	fakePut(TARGET, BODY);
}

/*** name selection (§3.21.2) ***/

/* The first candidate is the path with a trailing tilde. */
void test_backup_default_name(void) {
	char bp[PATH_MAX];
	seedTarget();

	TEST_ASSERT_EQUAL_INT(0, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_STRING("/w/notes.txt~", bp);
	TEST_ASSERT_EQUAL_STRING(BODY, fakeContents(bp));
}

/* The backup holds the target's on-disk bytes, and the target itself
 * is left alone.  A backup that truncated what it was protecting
 * would be worse than no backup at all. */
void test_backup_preserves_target(void) {
	char bp[PATH_MAX];
	seedTarget();

	TEST_ASSERT_EQUAL_INT(0, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_STRING(BODY, fakeContents(TARGET));
	TEST_ASSERT_EQUAL_INT(0, n_unlink_calls);
}

/* An existing backup must never be overwritten: the final character of
 * the path is replaced by a letter, from 'z' downward. */
void test_backup_falls_back_to_z(void) {
	char bp[PATH_MAX];
	seedTarget();
	fakePut("/w/notes.txt~", "an older backup");

	TEST_ASSERT_EQUAL_INT(0, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_STRING("/w/notes.txz~", bp);
	/* the pre-existing backup is untouched */
	TEST_ASSERT_EQUAL_STRING("an older backup",
				 fakeContents("/w/notes.txt~"));
}

/* Candidates walk z, y, x ... in order. */
void test_backup_walks_letters_downward(void) {
	char bp[PATH_MAX];
	seedTarget();
	fakePut("/w/notes.txt~", "x");
	fakePut("/w/notes.txz~", "x");
	fakePut("/w/notes.txy~", "x");

	TEST_ASSERT_EQUAL_INT(0, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_STRING("/w/notes.txx~", bp);
}

/* With all 27 candidates taken there is no safe name, so the backup
 * fails rather than clobbering one. */
void test_backup_exhausts_candidates(void) {
	char bp[PATH_MAX];
	char name[PATH_MAX];
	seedTarget();

	fakePut("/w/notes.txt~", "x");
	for (int c = 'z'; c >= 'a'; c--) {
		snprintf(name, sizeof(name), "/w/notes.tx%c~", c);
		fakePut(name, "x");
	}

	TEST_ASSERT_EQUAL_INT(-1, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(EEXIST, errno);
}

/* A buffer too small for path + '~' + NUL is ENAMETOOLONG, not a
 * truncated backup name pointing at the wrong file. */
void test_backup_name_too_long(void) {
	char bp[8];
	seedTarget();

	TEST_ASSERT_EQUAL_INT(-1, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(ENAMETOOLONG, errno);
}

/* The fallback replaces a whole final codepoint, so a multi-byte tail
 * is not cut in half. */
void test_backup_multibyte_tail(void) {
	char bp[PATH_MAX];
	fakeReset();
	fakeMkdir("/w");
	fakePut("/w/né", BODY);      /* 'é' is two bytes */
	fakePut("/w/né~", "taken"); /* force the fallback */

	TEST_ASSERT_EQUAL_INT(0, fake_makeVerifiedBackup("/w/né", bp,
							 sizeof(bp)));
	TEST_ASSERT_EQUAL_STRING("/w/nz~", bp);
}

/*** fault injection ***/

/* Every failure below must leave no partial backup on disk.  A
 * half-written backup is indistinguishable from a good one at the
 * moment the caller truncates the target. */

void test_backup_read_failure_removes_partial(void) {
	char bp[PATH_MAX];
	seedTarget();
	f_read.n_fail = 1;
	f_read.err = EIO;

	TEST_ASSERT_EQUAL_INT(-1, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(EIO, errno);
	TEST_ASSERT_EQUAL_INT(0, fakeExists("/w/notes.txt~"));
	TEST_ASSERT_EQUAL_STRING("/w/notes.txt~", last_unlinked);
}

void test_backup_write_failure_removes_partial(void) {
	char bp[PATH_MAX];
	seedTarget();
	f_write.n_fail = 1;
	f_write.err = ENOSPC;

	TEST_ASSERT_EQUAL_INT(-1, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(ENOSPC, errno);
	TEST_ASSERT_EQUAL_INT(0, fakeExists("/w/notes.txt~"));
}

/* The fsync is the whole point of "verified": a backup that reached
 * only the page cache is not a backup. */
void test_backup_fsync_failure_removes_partial(void) {
	char bp[PATH_MAX];
	seedTarget();
	f_fsync.n_fail = 1;
	f_fsync.err = EIO;

	TEST_ASSERT_EQUAL_INT(-1, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(EIO, errno);
	TEST_ASSERT_EQUAL_INT(0, fakeExists("/w/notes.txt~"));
}

/* close() reports deferred write errors, so a failing close is a
 * failed backup even though every write returned success. */
void test_backup_close_failure_removes_partial(void) {
	char bp[PATH_MAX];
	seedTarget();
	/* calls: close(source fd), close(backup fd) */
	f_close.n_fail = 2;
	f_close.err = EIO;

	TEST_ASSERT_EQUAL_INT(-1, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(EIO, errno);
	TEST_ASSERT_EQUAL_INT(0, fakeExists("/w/notes.txt~"));
}

/* The target vanishing between the caller's stat and the open is a
 * real race, not a theoretical one. */
void test_backup_source_open_failure(void) {
	char bp[PATH_MAX];
	seedTarget();
	/* calls: open(backup, O_EXCL), open(target, O_RDONLY) */
	f_open.n_fail = 2;
	f_open.err = ENOENT;

	TEST_ASSERT_EQUAL_INT(-1, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(ENOENT, errno);
	TEST_ASSERT_EQUAL_INT(0, fakeExists("/w/notes.txt~"));
}

/* No failure path may leak a descriptor.  The editor is long-lived and
 * a save is a repeatable action, so a leak here is unbounded. */
void test_backup_no_fd_leaks_on_failure(void) {
	char bp[PATH_MAX];
	struct faultSpec *specs[] = { &f_read, &f_write, &f_fsync };
	const char *names[] = { "read", "write", "fsync" };

	for (int i = 0; i < 3; i++) {
		seedTarget();
		specs[i]->n_fail = 1;
		specs[i]->err = EIO;
		(void)runBackup(bp, sizeof(bp));
		if (fakeOpenFds() != 0)
			printf("  leaked %d fd(s) after %s failure\n",
			       fakeOpenFds(), names[i]);
		TEST_ASSERT_EQUAL_INT(0, fakeOpenFds());
	}
}

/* The success path must close everything too. */
void test_backup_no_fd_leaks_on_success(void) {
	char bp[PATH_MAX];
	seedTarget();

	TEST_ASSERT_EQUAL_INT(0, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(0, fakeOpenFds());
}

/* An empty target is a legitimate file, and its backup is an empty
 * file rather than an absent one. */
void test_backup_empty_target(void) {
	char bp[PATH_MAX];
	fakeReset();
	fakeMkdir("/w");
	fakePut(TARGET, "");

	TEST_ASSERT_EQUAL_INT(0, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(1, fakeExists(bp));
	TEST_ASSERT_EQUAL_STRING("", fakeContents(bp));
}

/* Contents longer than copyFd's 8192-byte buffer exercise the loop
 * rather than a single read/write pair. */
void test_backup_multi_chunk_copy(void) {
	char bp[PATH_MAX];
	char big[FAKE_MAX_DATA];
	memset(big, 'a', sizeof(big) - 1);
	big[sizeof(big) - 1] = '\0';

	fakeReset();
	fakeMkdir("/w");
	fakePut(TARGET, big);

	TEST_ASSERT_EQUAL_INT(0, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_STRING(big, fakeContents(bp));
}

/* A directory fsync that the filesystem does not support is not a
 * failure.  Note this asserts what the CODE does; §3.21.2 currently
 * claims emil never syncs the parent directory at all, and is stale
 * on this point. */
void test_backup_dir_sync_unsupported_is_ok(void) {
	char bp[PATH_MAX];
	seedTarget();
	/* fsync calls: backup fd, then the parent directory */
	f_fsync.n_fail = 2;
	f_fsync.err = ENOTSUP;

	TEST_ASSERT_EQUAL_INT(0, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(1, fakeExists(bp));
}

/* A genuine directory-sync failure is fatal, and takes the backup
 * with it: the entry may not have reached disk. */
void test_backup_dir_sync_eio_is_fatal(void) {
	char bp[PATH_MAX];
	seedTarget();
	f_fsync.n_fail = 2;
	f_fsync.err = EIO;

	TEST_ASSERT_EQUAL_INT(-1, runBackup(bp, sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(EIO, errno);
	TEST_ASSERT_EQUAL_INT(0, fakeExists("/w/notes.txt~"));
}

/* A NULL path is rejected rather than dereferenced. */
void test_backup_null_path(void) {
	char bp[PATH_MAX];
	fakeReset();

	TEST_ASSERT_EQUAL_INT(-1, fake_makeVerifiedBackup(NULL, bp,
							  sizeof(bp)));
	TEST_ASSERT_EQUAL_INT(EINVAL, errno);
}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_backup_default_name);
	RUN_TEST(test_backup_preserves_target);
	RUN_TEST(test_backup_falls_back_to_z);
	RUN_TEST(test_backup_walks_letters_downward);
	RUN_TEST(test_backup_exhausts_candidates);
	RUN_TEST(test_backup_name_too_long);
	RUN_TEST(test_backup_multibyte_tail);

	RUN_TEST(test_backup_read_failure_removes_partial);
	RUN_TEST(test_backup_write_failure_removes_partial);
	RUN_TEST(test_backup_fsync_failure_removes_partial);
	RUN_TEST(test_backup_close_failure_removes_partial);
	RUN_TEST(test_backup_source_open_failure);
	RUN_TEST(test_backup_no_fd_leaks_on_failure);
	RUN_TEST(test_backup_no_fd_leaks_on_success);

	RUN_TEST(test_backup_empty_target);
	RUN_TEST(test_backup_multi_chunk_copy);
	RUN_TEST(test_backup_dir_sync_unsupported_is_ok);
	RUN_TEST(test_backup_dir_sync_eio_is_fatal);
	RUN_TEST(test_backup_null_path);

	return TEST_END();
}
