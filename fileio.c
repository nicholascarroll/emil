/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include "fileio.h"
#include "buffer.h"
#include "dbuf.h"
#include "display.h"
#include "emil.h"
#include "keymap.h"

#include "mutate.h"
#include "prompt.h"
#include "terminal.h"
#include "undo.h"
#include "unicode.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/* Access global editor state */

/*** timed syscall support ***/

/* SIGALRM handler for timed_stat / timed_lockFile.  Interrupts any
 * blocking syscall (stat, open, fcntl) so checkFileModified never
 * stalls the editor on a slow or hung filesystem.
 *
 * The handler deliberately does nothing.  Delivery is the whole
 * mechanism: with no SA_RESTART the interrupted call returns EINTR,
 * and every caller below distinguishes that from a real failure
 * itself (lockFile's LOCK_RETRY, timed_stat's return).  It carried a
 * `file_check_timed_out` flag that was set here, cleared in
 * armTimer() and read nowhere -- state that looked like a protocol
 * and was not one. */
static void fileCheckAlarm(int sig) {
	(void)sig;
}

/* Install the SIGALRM handler.  Called once from main. */
void initFileCheck(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = fileCheckAlarm;
	sa.sa_flags = 0; /* no SA_RESTART: we want EINTR */
	sigaction(SIGALRM, &sa, NULL);
}

/* Arm a 50ms one-shot timer.  Returns 0 on success. */
static int armTimer(void) {
	struct itimerval it;
	memset(&it, 0, sizeof(it));
	it.it_value.tv_usec = 50000; /* 50ms */
	return setitimer(ITIMER_REAL, &it, NULL);
}

/* Disarm the timer. */
static void disarmTimer(void) {
	struct itimerval it;
	memset(&it, 0, sizeof(it));
	setitimer(ITIMER_REAL, &it, NULL);
}

/* How many seconds between file-check syscalls. */
#define FILE_CHECK_INTERVAL_SEC 2

/* Force the next checkFileModified call to run immediately,
 * bypassing the throttle.  Called on events where the user's
 * context has changed and stale state should be caught promptly:
 * buffer switch, resume from suspend (fg). */
void resetFileCheckThrottle(void) {
	memset(&E.last_file_check, 0, sizeof(E.last_file_check));
}

/*** file locking ***/

/* POSIX advisory record locking is not universally available.  WASIX
 * (and wasi-libc generally) declares struct flock and the l_type
 * values but deliberately omits the F_GETLK/F_SETLK commands, because
 * there is no host lock manager behind them -- see the
 * __wasilibc_unmodified_upstream guards in its <fcntl.h>.  Detect that
 * by the absence of the command constants rather than by testing for a
 * specific platform macro, so any libc making the same choice is
 * handled without further edits here.
 *
 * The fallback is not a new policy: it routes to the same conclusions
 * the runtime ENOLCK path already reaches on a POSIX host whose lock
 * manager is unavailable.  See the two stubs below for why each picks
 * the value it does. */
#if !defined(F_GETLK) || !defined(F_SETLK)
#define EMIL_NO_FILE_LOCKING 1
#endif

/* Probe whether an advisory lock is held on a file without acquiring one.
 * Returns 0 if no lock is held, PID if locked , -1 if unknown
 * or -2 on error (file doesn't exist, can't open, etc.). */
#ifdef EMIL_NO_FILE_LOCKING
int probeLock(const char *filename) {
	/* 0 ("no lock held"), not -1 ("held, holder unknown").  With no
	 * lock manager on the platform, no process can be holding an
	 * advisory lock, so 0 is the honest answer rather than a
	 * convenient one.  The distinction is load-bearing:
	 * editorOpen() treats -1 as a real lock and opens the buffer
	 * read-only, which would make every file in the session
	 * unwritable. */
	(void)filename;
	return 0;
}
#else
int probeLock(const char *filename) {
	int fd = open(filename, O_RDONLY);
	if (fd < 0)
		return -2;

	struct flock query;
	memset(&query, 0, sizeof(query));
	query.l_type = F_WRLCK;
	query.l_whence = SEEK_SET;
	query.l_start = 0;
	query.l_len = 0;
	int pid = 0;
	if (fcntl(fd, F_GETLK, &query) == 0 && query.l_type != F_UNLCK) {
		pid = (int)query.l_pid;
	}
	close(fd);
	return pid;
}
#endif /* EMIL_NO_FILE_LOCKING */

/* Try to acquire an advisory write lock on a file.  Returns one of
 * the enum lockResult values (see fileio.h).  On LOCK_ACQUIRED,
 * bufr->lock_fd is set and must be released later.  On LOCK_CONFLICT,
 * bufr->lock_blocked_pid names the holder (or is -1 if F_GETLK does
 * not name one).  No other outcome touches lock_blocked_pid. */
#ifdef EMIL_NO_FILE_LOCKING
int lockFile(struct buffer *bufr, const char *filename) {
	/* LOCK_UNAVAILABLE is the ENOLCK case: nothing to acquire and
	 * nothing to wait for, so the background poll stops asking
	 * instead of retrying a lock manager that will never exist.
	 * lock_fd stays -1, which keeps releaseLock a no-op, and
	 * lock_blocked_pid is left untouched per the contract above. */
	(void)bufr;
	(void)filename;
	return LOCK_UNAVAILABLE;
}
#else
int lockFile(struct buffer *bufr, const char *filename) {
	/* Try O_RDWR first (needed for F_WRLCK per POSIX).
	 * Fall back to O_RDONLY + F_RDLCK if the file isn't writable. */
	int fd = open(filename, O_RDWR);
	int use_rdlck = 0;
	if (fd < 0) {
		if (errno == ENOENT)
			return LOCK_UNAVAILABLE; /* nothing to lock */
		/* EINTR is not a permission failure: the background check
		 * arms a SIGALRM deadline around this very call, and
		 * falling through to the O_RDONLY retry would take an
		 * F_RDLCK on a writable file and report it unwritable. */
		if (errno == EINTR)
			return LOCK_RETRY;
		fd = open(filename, O_RDONLY);
		if (fd < 0)
			return errno == EINTR ? LOCK_RETRY : LOCK_UNAVAILABLE;
		use_rdlck = 1;
	}

	struct flock fl;
	memset(&fl, 0, sizeof(fl));
	fl.l_type = use_rdlck ? F_RDLCK : F_WRLCK;
	fl.l_whence = SEEK_SET;
	fl.l_start = 0;
	fl.l_len = 0; /* whole file */

	if (fcntl(fd, F_SETLK, &fl) == 0) {
		/* Lock acquired */
		bufr->lock_fd = fd;

		/* open_mtime is deliberately NOT set here.  This runs
		 * on the clean->dirty edge, so re-baselining would
		 * adopt whatever another process wrote while the
		 * buffer sat clean, and the pre-save check (§3.21.2)
		 * would then find no drift.  relockAll() avoids
		 * lockFile() for the same reason.  Load, save and
		 * revert set the baseline. */

		return LOCK_ACQUIRED;
	}

	int lock_errno = errno;

	/* A genuine conflict: find out who holds it and record that on
	 * the buffer for the persistent status-bar warning. */
	if (lock_errno == EACCES || lock_errno == EAGAIN) {
		struct flock query;
		memset(&query, 0, sizeof(query));
		query.l_type = F_WRLCK;
		query.l_whence = SEEK_SET;
		query.l_start = 0;
		query.l_len = 0;

		if (fcntl(fd, F_GETLK, &query) == 0 &&
		    query.l_type != F_UNLCK) {
			bufr->lock_blocked_pid = (int)query.l_pid;
		} else {
			bufr->lock_blocked_pid = -1; /* unknown holder */
		}
		close(fd);
		return LOCK_CONFLICT;
	}

	close(fd);
	if (lock_errno == EINTR)
		return LOCK_RETRY;

	/* ENOLCK and friends: locking is not available here at all.
	 * There is no holder to wait for, so the caller must stop
	 * asking rather than probe a dead lock manager forever. */
	return LOCK_UNAVAILABLE;
}
#endif /* EMIL_NO_FILE_LOCKING */

/* Release the advisory lock held by this buffer.
 *
 * Deliberately does NOT clear open_mtime.  The lock lifetime and the
 * external-modification baseline are unrelated: this runs on the
 * dirty->clean edge, which a plain run of C-_ back to the start of the
 * session reaches, and checkFileModified's job 1 is gated on
 * open_mtime != 0.  Callers that genuinely change which file the buffer
 * refers to (saveAs) clear the baseline themselves. */
void releaseLock(struct buffer *bufr) {
	if (bufr->lock_fd >= 0) {
		close(bufr->lock_fd);
		bufr->lock_fd = -1;
		/* That close dropped every lock this process holds on
		 * the inode, not just this buffer's (see relockAll): a
		 * second dirty buffer visiting the same file through a
		 * symlink holds its own lock_fd but shares the one
		 * per-process lock record, so releasing here -- from
		 * markBufferClean() on save, or destroyBuffer() on
		 * kill -- silently unlocked it too.  Re-assert.  When
		 * no sibling exists this finds nothing to do, so the
		 * release still releases. */
		relockAll();
	}
	bufr->lock_blocked_pid = 0;
}

/* Re-assert every advisory lock we believe we hold.
 *
 * POSIX record locks are owned by the process, not the descriptor:
 * closing *any* descriptor referring to an inode drops every lock the
 * process holds on it (APUE §14.3).  So an operation as ordinary as
 * `C-x i` on the file the buffer is already visiting -- one fopen()
 * and one fclose() -- silently released a lock taken by
 * markBufferDirty(), while lock_fd stayed open so emil went on
 * believing it held one.  Reproduced on Linux/glibc for `C-x i` and
 * for `C-x C-f` through a symlink to an open file; the mechanism is
 * the standard's, not a platform's.
 *
 * Nothing inside emil can notice this, because F_GETLK reports
 * F_UNLCK for a lock the calling process holds itself: the only
 * observer is the second emil instance that was supposed to be warned
 * off, and it is warned off no longer.  Repair rather than
 * prevention, because the only way to keep a lock across an unrelated
 * close is an open-file-description lock (F_OFD_SETLK), which is
 * Linux-only and outside the POSIX.1-2001 baseline (§1.2).
 *
 * Re-issued on the *retained* descriptor rather than by calling
 * lockFile() again, for two reasons.  lockFile() opens a fresh
 * descriptor, so the old one would have to be closed afterwards --
 * which would drop the lock just acquired.  And lockFile() resets
 * open_mtime from its fstat, which would silently move the
 * external-modification baseline (§3.21.4) on an operation that never
 * touched the file's contents. */
#ifdef EMIL_NO_FILE_LOCKING
void relockAll(void) {
	/* lockFile() never sets lock_fd on this platform, so no buffer
	 * can be holding a lock to re-assert. */
}
#else
void relockAll(void) {
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
		if (b->lock_fd < 0)
			continue;

		/* Which lock we took is recoverable from the descriptor
		 * we took it on: lockFile() falls back to O_RDONLY +
		 * F_RDLCK when the file is not writable, so the access
		 * mode and the lock type agree by construction.  Asking
		 * the fd avoids a second copy of that decision on the
		 * buffer, which could then disagree with it. */
		int mode = fcntl(b->lock_fd, F_GETFL);
		struct flock fl;
		memset(&fl, 0, sizeof(fl));
		fl.l_type = (mode >= 0 && (mode & O_ACCMODE) == O_RDWR) ?
				    F_WRLCK :
				    F_RDLCK;
		fl.l_whence = SEEK_SET;
		fl.l_start = 0;
		fl.l_len = 0; /* whole file */

		if (fcntl(b->lock_fd, F_SETLK, &fl) == 0)
			continue; /* still ours, or ours again */

		int lock_errno = errno;

		/* We do not hold it, whatever the reason, so lock_fd
		 * must stop claiming otherwise.  Dropping it to -1 is
		 * also what re-arms the background re-probe, which is
		 * gated on lock_blocked_pid != 0 && lock_fd < 0
		 * (§3.21.3). */
		if (lock_errno == EACCES || lock_errno == EAGAIN) {
			/* Another process took it in the window between
			 * the close and here.  Name the holder and route
			 * into the existing LOCK_CONFLICT presentation
			 * rather than inventing a second one. */
			struct flock query;
			memset(&query, 0, sizeof(query));
			query.l_type = F_WRLCK;
			query.l_whence = SEEK_SET;
			query.l_start = 0;
			query.l_len = 0;
			if (fcntl(b->lock_fd, F_GETLK, &query) == 0 &&
			    query.l_type != F_UNLCK)
				b->lock_blocked_pid = (int)query.l_pid;
			else
				b->lock_blocked_pid = -1;
			close(b->lock_fd);
			b->lock_fd = -1;
		} else {
			/* ENOLCK and friends: there is no holder to wait
			 * for.  Same conclusion lockFile() draws with
			 * LOCK_UNAVAILABLE -- leave lock_blocked_pid
			 * alone so the poll does not chase a lock
			 * manager that is not there. */
			close(b->lock_fd);
			b->lock_fd = -1;
		}
	}
}
#endif /* EMIL_NO_FILE_LOCKING */

/* The advisory lock is no longer held by anyone else -- or can no
 * longer be determined at all.  Clear the warning, and lift a
 * read-only that we imposed for the lock.  A read-only set because
 * access(W_OK) failed, or because the user pressed C-x C-q, is not
 * ours to undo and is left alone. */
static void clearLockWarning(struct buffer *bufr) {
	bufr->lock_blocked_pid = 0;
	if (bufr->read_only_by_lock) {
		bufr->read_only = 0;
		bufr->read_only_by_lock = 0;
	}
}

/* Check whether the underlying file has been modified externally, and
 * opportunistically clear a stale lock_blocked_pid warning if the
 * blocking process has since released the lock.
 *
 * Called periodically (from refreshScreen) on the focused buffer, at
 * most once every FILE_CHECK_INTERVAL_SEC seconds on the monotonic
 * clock, with each stat() / lockFile() wrapped in a 50ms SIGALRM
 * deadline so a hung filesystem never stalls the editor.
 *
 * Two independent jobs:
 *
 *   1. Set bufr->external_mod if mtime has drifted since open/save.
 *      One-shot: skipped if the flag is already set.
 *
 *   2. Clear a stale lock warning once the holder has released.  Runs
 *      while lock_blocked_pid is set and we do not hold the lock,
 *      whether or not the buffer is dirty -- a buffer opened read-only
 *      BECAUSE of a lock can never become dirty, so a dirty gate would
 *      exclude the most common way the warning appears.
 *
 *      A dirty buffer wants the lock, so it tries to acquire it.  A
 *      clean one only wants to know whether the holder is gone, so it
 *      probes without acquiring.
 *
 *      Gated on !external_mod: if the blocking process saved before
 *      releasing, Job 1 has already set external_mod, and we
 *      deliberately don't take a lock we'd use to overwrite those
 *      changes.
 *
 * The timeout is advisory in BOTH jobs and never invalidates a result:
 * if the syscall returned successfully, that answer is used even though
 * the alarm fired during it.  The flag says the operation was slow, not
 * that its result is wrong. */

void checkFileModified(void) {
	if (E.buf->filename == NULL || E.buf->special_buffer)
		return;

	/* Throttle: skip if we checked recently.  Fails closed -- if
	 * the clock is unreadable we skip the check rather than run it
	 * unthrottled on every frame. */
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;
	long elapsed = now.tv_sec - E.last_file_check.tv_sec;
	if (elapsed >= 0 && elapsed < FILE_CHECK_INTERVAL_SEC)
		return;
	E.last_file_check = now;

	/* Job 1: mtime check. */
	if (E.buf->open_mtime != 0 && !E.buf->external_mod) {
		char *iopath = expandTilde(E.buf->filename);
		struct stat st;
		armTimer();
		int rc = stat(iopath, &st);
		disarmTimer();
		if (rc == 0 && (st.st_mtime != E.buf->open_mtime ||
				st.st_size != E.buf->open_size)) {
			E.buf->external_mod = 1;
		}
		free(iopath);
	}

	/* Job 2: stale-lock clearing. */
	if (E.buf->lock_blocked_pid != 0 && E.buf->lock_fd < 0 &&
	    !E.buf->external_mod) {
		char *iopath = expandTilde(E.buf->filename);
		if (E.buf->dirty) {
			/* We want the lock: try to take it. */
			armTimer();
			int rc = lockFile(E.buf, iopath);
			disarmTimer();
			if (rc == LOCK_ACQUIRED || rc == LOCK_UNAVAILABLE) {
				/* Acquired, or there is nothing here to
				 * wait for.  Either way the warning is
				 * no longer true; stop repeating it and,
				 * for LOCK_UNAVAILABLE, stop asking.
				 *
				 * Silent: the user did not ask to take a
				 * lock, and announcing it from a
				 * background poll would clobber whatever
				 * message they were reading. */
				clearLockWarning(E.buf);
			}
			/* LOCK_CONFLICT: lockFile has refreshed
			 * lock_blocked_pid to the current holder, which
			 * may be a different process from before.
			 * LOCK_RETRY: leave the state alone and try
			 * again on the next tick. */
		} else {
			/* We do not want the lock, only to know whether
			 * it is still held. */
			armTimer();
			int pid = probeLock(iopath);
			disarmTimer();
			if (pid == 0 || pid == -2) {
				/* Free, or no longer answerable. */
				clearLockWarning(E.buf);
			} else {
				E.buf->lock_blocked_pid = pid;
			}
		}
		/* Both branches opened and closed a descriptor on this
		 * buffer's inode -- probeLock() always, lockFile() on
		 * its failure paths -- and either close drops any lock
		 * another buffer holds on the same inode through a
		 * symlink (see relockAll).  This is the timer-driven
		 * copy of the keystroke-driven drops fixed at
		 * editorOpen and insertFileAtPath: the probe answers
		 * "no lock held" precisely because F_GETLK cannot see
		 * our own, so it both causes the drop and reports
		 * nothing wrong. */
		relockAll();
		free(iopath);
	}
}

/*** file i/o ***/

/* Serialise the buffer: rows joined by '\n', with no terminator.  A
 * trailing newline in the output comes from a trailing empty row, not
 * from this function -- see the invariant note in buffer.h.  The
 * result is NUL-terminated for convenience; *buflen excludes it.
 *
 * Byte-exact against editorOpen for LF files.  CRLF input is
 * converted to LF on load and written back as LF; preserving CRLF is
 * a separate question. */
char *rowsToString(struct buffer *bufr, size_t *buflen) {
	size_t totlen = 0;
	int j;
	for (j = 0; j < bufr->numrows; j++)
		totlen += bufr->row[j].size;
	if (bufr->numrows > 1)
		totlen += (size_t)bufr->numrows - 1;
	*buflen = totlen;

	char *buf = xmalloc(totlen + 1);
	char *p = buf;
	for (j = 0; j < bufr->numrows; j++) {
		if (j > 0) {
			*p = '\n';
			p++;
		}
		memcpy(p, bufr->row[j].chars, bufr->row[j].size);
		p += bufr->row[j].size;
	}
	*p = '\0';

	return buf;
}

/* Validate UTF-8 in the buffer and check for null bytes.
 * Also rejects overlong encodings, surrogates (U+D800-U+DFFF),
 * and codepoints above U+10FFFF.
 * Returns 1 if valid, 0 if invalid. */

static int checkUTF8Validity(struct buffer *bufr) {
	for (int row = 0; row < bufr->numrows; row++) {
		if (!utf8_validate(bufr->row[row].chars, bufr->row[row].size))
			return 0;
	}
	return 1;
}

/* Pre-scan an open file for null bytes.  Returns 1 if null bytes
 * are found, 0 if clean.  Rewinds the file on return.
 * This is needed because emil_getline uses fgets/strlen internally,
 * which treats '\0' as a string terminator and would silently
 * truncate lines containing null bytes. */

static int fileContainsNullBytes(FILE *fp) {
	unsigned char buf[8192];
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), fp)) > 0) {
		if (memchr(buf, '\0', n) != NULL) {
			rewind(fp);
			return 1;
		}
	}
	rewind(fp);
	return 0;
}

/* Open a file into a buffer.
 * Returns 0 on success, -1 on failure (file not found is not a failure;
 * the buffer is left empty with the filename set). */

static int editorOpenBody(struct buffer *bufr, const char *filename);

/* The wrapper exists so that relockAll() cannot be bypassed.  The body
 * below opens the file, and so drops any lock this process holds on
 * that inode (see relockAll), on every one of its exits -- including
 * the failure exits, where the buffer that lost its lock is a
 * different one that is not being touched at all.  A call placed after
 * each `return` inside would be correct today and wrong the first time
 * someone adds an early return, which is exactly the kind of silent
 * data-safety regression this repairs. */
int editorOpen(struct buffer *bufr, const char *filename) {
	int rc = editorOpenBody(bufr, filename);
	relockAll();
	return rc;
}

static int editorOpenBody(struct buffer *bufr, const char *filename) {
	free(bufr->filename);
	bufr->filename = collapseHome(filename);

	/* Resolve to an OS-usable path for all I/O in this function */
	char *iopath = expandTilde(bufr->filename);

	FILE *fp = fopen(iopath, "r");
	if (!fp) {
		if (errno == ENOENT) {
			/* A path with a trailing '/' names a directory;
			 * it can never be created as a regular file, so
			 * don't offer it as a "new file". */
			size_t plen = strlen(iopath);
			if (plen > 0 && iopath[plen - 1] == '/') {
				setStatusMessage("Can't open file: %s",
						 strerror(EISDIR));
				free(bufr->filename);
				bufr->filename = NULL;
				free(iopath);
				return -1;
			}
			setStatusMessage("%s (New file)", bufr->filename);
			free(iopath);
			return 0;
		}
		setStatusMessage("Can't open file: %s", strerror(errno));
		free(bufr->filename);
		bufr->filename = NULL;
		free(iopath);
		return -1;
	}

	/* Reject directories (fopen(dir, "r") succeeds on many
	 * systems and reads then silently fail with EISDIR, which
	 * would present the directory as an empty new buffer) and
	 * check regular files against the hard size limit. */
	{
		struct stat st;
		if (fstat(fileno(fp), &st) == 0) {
			if (S_ISDIR(st.st_mode)) {
				fclose(fp);
				setStatusMessage("Can't open file: %s",
						 strerror(EISDIR));
				free(bufr->filename);
				bufr->filename = NULL;
				free(iopath);
				return -1;
			}
			if (S_ISREG(st.st_mode) &&
			    (size_t)st.st_size > EMIL_MAX_FILE_SIZE) {
				fclose(fp);
				setStatusMessage("Exceeds 1 GiB limit");
				free(bufr->filename);
				bufr->filename = NULL;
				free(iopath);
				return -1;
			}
		}
	}

	/* Pre-scan for null bytes before line-based reading because
	 * emil_getline (fgets/strlen) silently truncates at '\0'. */
	if (fileContainsNullBytes(fp)) {
		fclose(fp);
		setStatusMessage("File contains null bytes (binary file?)");
		free(bufr->filename);
		bufr->filename = NULL;
		free(iopath);
		return -1;
	}

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	/* Rebuild the row array from scratch: the buffer arrives from
	 * newBuffer (or a previous load, via revert) already holding
	 * rows, and the file's content replaces them wholesale.  The
	 * invariant is restored below, before returning. */
	bufferResetRows(bufr);

	/* A text file is lines each terminated by '\n'.  Input departing
	 * from that is normalised on the way in, and the user told, so
	 * the file does not quietly change at save. */
	int dos_endings = 0;
	int no_final_newline = 0;

	while ((linelen = emil_getline(&line, &linecap, fp)) != -1) {
		/* Sampled before stripping and overwritten each pass, so
		 * after the loop it describes the final line. */
		no_final_newline = (linelen == 0 || line[linelen - 1] != '\n');

		/* A CR counts as a DOS ending only when it precedes the
		 * '\n'.  A lone CR is an ordinary byte, kept as-is on
		 * save, so flagging it would promise a conversion that
		 * never happens. */
		if (linelen >= 2 && line[linelen - 1] == '\n' &&
		    line[linelen - 2] == '\r')
			dos_endings = 1;

		while (linelen > 0 &&
		       (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen--;
		appendRowRaw(bufr, (const uint8_t *)line, linelen);
	}

	/* The file is the rows joined by '\n', so a trailing newline is
	 * one more (empty) row.  Appended unconditionally: emil's buffers
	 * end in a newline (see mutate.c), so a file that arrives without
	 * one gains it here rather than at save time.  The invariant then
	 * holds from load onwards and save has no policy to apply.
	 *
	 * appendRowRaw deliberately does not dirty the buffer, so this
	 * costs nothing: an untouched file is still clean, still prompts
	 * nothing on quit, and is not rewritten unless the user edits it.
	 *
	 * An empty file is the single empty row, which serialises back to
	 * zero bytes -- it has no lines, so it has nothing to terminate. */
	appendRowRaw(bufr, (const uint8_t *)"", 0);

	/* Get the display length of the longest column */
	int max_width = 0;
	for (int i = 0; i < bufr->numrows; i++) {
		int w = calculateLineWidth(&bufr->row[i]);
		if (w > max_width)
			max_width = w;
	}

	free(line);
	fclose(fp);

	/* Guard against pathological files with billions of tiny lines. */
	if (bufr->numrows > INT_MAX / 2) {
		bufferResetRows(bufr);
		appendRowRaw(bufr, (const uint8_t *)"", 0);
		free(bufr->filename);
		bufr->filename = NULL;
		setStatusMessage("File has too many lines");
		free(iopath);
		return -1;
	}

	/* Validate UTF-8 encoding of the loaded content */
	if (!checkUTF8Validity(bufr)) {
		bufferResetRows(bufr);
		appendRowRaw(bufr, (const uint8_t *)"", 0);
		free(bufr->filename);
		bufr->filename = NULL;
		setStatusMessage("Failed UTF-8 validation");
		free(iopath);
		return -1;
	}

	/* The load used appendRowRaw which deliberately does not dirty
	 * the buffer or invalidate per-row; invalidate the screen cache
	 * once here now that all rows are in place.  The buffer is
	 * already clean (newBuffer initialized it that way, and the
	 * load did not touch dirty state), so no markBufferClean is
	 * needed. */

	if (access(iopath, W_OK) != 0) {
		bufr->read_only = 1;
	}

	/* Record mtime for external-modification detection.  The lock is
	 * not taken here: it is held only while the buffer is dirty, so
	 * acquisition is deferred to markBufferDirty(). */
	{
		struct stat st;
		if (stat(iopath, &st) == 0) {
			bufr->open_mtime = st.st_mtime;
			bufr->open_size = st.st_size;
		}
	}

	/* Probe for an advisory lock held by another process.  If one
	 * is found, open the buffer read-only so the user doesn't
	 * accidentally collide with the other editor instance. */
	int lock_pid = probeLock(iopath);

	free(iopath);

	computeDisplayNames();

	/* Enable word wrap by default for prose-oriented file types */
	if (bufr->filename) {
		char *ext = strrchr(bufr->filename, '.');
		if (ext) {
			if (strcmp(ext, ".org") == 0 ||
			    strcmp(ext, ".md") == 0 ||
			    strcmp(ext, ".txt") == 0 ||
			    strcmp(ext, ".fountain") == 0) {
				bufr->word_wrap = 1;
			}
		}
	}

	/* probeLock returns -2 on error (unreadable, vanished), which is
	 * not a lock and must not reach lock_blocked_pid: the status bar
	 * would render it as "-2 LOCK". */
	if (lock_pid > 0 || lock_pid == -1) {
		/* Only claim the read-only as ours if nothing else has
		 * already imposed it -- otherwise releasing the lock
		 * would make an unwritable file writable. */
		if (!bufr->read_only)
			bufr->read_only_by_lock = 1;
		bufr->read_only = 1;
		bufr->lock_blocked_pid = lock_pid;
		if (lock_pid > 0)
			setStatusMessage("Read only: advisory lock by PID %d",
					 lock_pid);
		else
			setStatusMessage(
				"Read only: advisory lock by another process");
	} else if (dos_endings && no_final_newline) {
		setStatusMessage("%d lines, %d columns; DOS line endings and "
				 "no final newline, both fixed on save",
				 bufferLineCount(bufr), max_width);
	} else if (dos_endings) {
		setStatusMessage("%d lines, %d columns; DOS line endings, "
				 "will be converted to Unix on save",
				 bufferLineCount(bufr), max_width);
	} else if (no_final_newline) {
		setStatusMessage("%d lines, %d columns; no final newline, "
				 "one will be added on save",
				 bufferLineCount(bufr), max_width);
	} else {
		setStatusMessage("%d lines, %d columns", bufferLineCount(bufr),
				 max_width);
	}
	return 0;
}

void revert(void) {
	struct buffer *buf = E.buf;

	/* A buffer that isn't visiting a file has nothing to revert to.
	 * editorOpen's first act is collapseHome(filename), which reads
	 * path[0] unconditionally, so passing NULL here crashed --
	 * reachable simply by starting emil with no arguments. */
	if (buf->filename == NULL) {
		setStatusMessage("Buffer is not visiting a file");
		return;
	}

	/* editorOpen returns 0 both when it loaded a file and when the
	 * file does not exist (ENOENT posts "(New file)"), so a "< 0"
	 * test cannot tell the two apart.  Without this check, reverting
	 * a buffer whose file was never written replaces it with an empty
	 * one and the destroyBuffer() below frees the undo stack, putting
	 * the work beyond recovery. */
	char *iopath = expandTilde(buf->filename);
	struct stat rst;
	if (stat(iopath, &rst) != 0) {
		setStatusMessage("File %s no longer exists!", buf->filename);
		free(iopath);
		return;
	}
	free(iopath);

	struct buffer *new = newBuffer();
	if (editorOpen(new, buf->filename) < 0) {
		/* Open/validation failed: keep the current buffer */
		destroyBuffer(new);
		return;
	}
	new->next = buf->next;
	E.buf = new;
	if (E.headbuf == buf) {
		E.headbuf = new;
	}
	struct buffer *cur = E.headbuf;
	while (cur != NULL) {
		if (cur->next == buf) {
			cur->next = new;
			break;
		}
		cur = cur->next;
	}
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->buf == buf) {
			E.windows[i]->buf = new;
		}
	}
	new->cx = buf->cx;
	new->cy = buf->cy;
	/* The cursor is carried over from the old buffer, so it may sit
	 * past the end of a file that shrank on disk.  Live guard, not a
	 * virtual-EOF leftover. */
	if (new->cy >= new->numrows) {
		new->cy = new->numrows - 1;
		new->cx = 0;
	} else if (new->cx > new->row[new->cy].size) {
		new->cx = new->row[new->cy].size;
	}
	destroyBuffer(buf);
}

/*** Backup and Write Strategy (Vim backupcopy=yes style) ***/

static int create_backup_exclusive(const char *name) {
	int fd;
	do {
		fd = open(name, O_WRONLY | O_CREAT | O_EXCL, 0600);
	} while (fd == -1 && errno == EINTR);

	if (fd != -1)
		(void)fcntl(fd, F_SETFD, FD_CLOEXEC);

	return fd;
}

static int backup_create(const char *path, char *backup, size_t backup_size) {
	size_t len;
	size_t char_start;
	size_t new_len;
	int fd;

	if (path == NULL || backup == NULL) {
		errno = EINVAL;
		return -1;
	}

	len = strlen(path);
	if (len == 0) {
		errno = EINVAL;
		return -1;
	}

	if (len + 2 > backup_size) {
		errno = ENAMETOOLONG;
		return -1;
	}

	memcpy(backup, path, len);
	backup[len] = '~';
	backup[len + 1] = '\0';

	fd = create_backup_exclusive(backup);
	if (fd != -1)
		return fd;

	if (errno != EEXIST)
		return -1;

	char_start = len;
	while (char_start > 0 && utf8_isCont((uint8_t)path[char_start - 1]))
		char_start--;

	new_len = char_start + 1;
	if (new_len + 2 > backup_size) {
		errno = ENAMETOOLONG;
		return -1;
	}

	memcpy(backup, path, char_start);
	backup[new_len] = '~';
	backup[new_len + 1] = '\0';

	for (int c = 'z'; c >= 'a'; c--) {
		backup[char_start] = (char)c;

		fd = create_backup_exclusive(backup);
		if (fd != -1)
			return fd;

		if (errno != EEXIST)
			return -1;
	}

	errno = EEXIST;
	return -1;
}

/*
 * Write all bytes to fd, retrying on EINTR and handling short writes.
 */
static int write_all(int fd, const char *buf, size_t len) {
	size_t total = 0;

	while (total < len) {
		ssize_t n = write(fd, buf + total, len - total);

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (n == 0) {
			errno = EIO;
			return -1;
		}

		total += (size_t)n;
	}

	return 0;
}

/*
 * Copy all bytes from from_fd to to_fd.
 */
static int copy_fd(int from_fd, int to_fd) {
	char buf[8192];

	while (1) {
		ssize_t n = read(from_fd, buf, sizeof(buf));

		if (n < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}

		if (n == 0)
			return 0;

		size_t off = 0;

		while (off < (size_t)n) {
			ssize_t w = write(to_fd, buf + off, (size_t)n - off);

			if (w < 0) {
				if (errno == EINTR)
					continue;
				return -1;
			}

			if (w == 0) {
				errno = EIO;
				return -1;
			}

			off += (size_t)w;
		}
	}
}

/*
 * Create a backup of path using backup_create(), then verify that the
 * backup was fully written, fsynced, and closed.
 *
 * On failure, remove the partial backup and return -1.
 */
static int make_verified_backup(const char *path, char *backup_path,
				size_t backup_path_size) {
	int bfd;
	int fd;
	int saved_errno;

	bfd = backup_create(path, backup_path, backup_path_size);
	if (bfd == -1)
		return -1;

	fd = open(path, O_RDONLY);
	if (fd == -1) {
		saved_errno = errno;
		close(bfd);
		unlink(backup_path);
		errno = saved_errno;
		return -1;
	}

	if (copy_fd(fd, bfd) == -1) {
		saved_errno = errno;
		close(fd);
		close(bfd);
		unlink(backup_path);
		errno = saved_errno;
		return -1;
	}

	if (close(fd) == -1) {
		saved_errno = errno;
		close(bfd);
		unlink(backup_path);
		errno = saved_errno;
		return -1;
	}

	if (fsync(bfd) == -1) {
		saved_errno = errno;
		close(bfd);
		unlink(backup_path);
		errno = saved_errno;
		return -1;
	}

	if (close(bfd) == -1) {
		saved_errno = errno;
		unlink(backup_path);
		errno = saved_errno;
		return -1;
	}

	return 0;
}

/*
 * Open the target file, truncate it, write the new contents, fsync it,
 * and close it.
 *
 * If require_regular is true, fail if the target descriptor is not a
 * regular file.  This is used when a backup exists, because deleting a
 * backup after writing to a non-regular file would be unsafe.
 *
 * On failure, set *damaged if the target file may now be damaged.
 */
static int write_in_place(const char *path, const char *buf, size_t len,
			  int require_regular, int *damaged) {
	int fd;
	struct stat st;
	int saved_errno;

	if (damaged)
		*damaged = 0;

	fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1)
		return -1;

	if (write_all(fd, buf, len) == -1)
		goto fail;

	if (fstat(fd, &st) == -1)
		goto fail;

	if (S_ISREG(st.st_mode)) {
		if (ftruncate(fd, (off_t)len) == -1)
			goto fail;

		if (fsync(fd) == -1)
			goto fail;
	} else if (require_regular) {
		errno = EIO;
		goto fail;
	}

	if (close(fd) == -1) {
		if (damaged)
			*damaged = 1;
		return -1;
	}

	return 0;

fail:
	saved_errno = errno;

	if (damaged)
		*damaged = 1;

	close(fd);
	errno = saved_errno;
	return -1;
}

/* Ask a y/N question in the minibuffer.  Returns 1 for yes. */
static int confirmYN(const char *msg) {
	setStatusMessage("%s", msg);
	refreshScreen();
	int c = readKey();
	clearStatusMessage();
	return (c == 'y' || c == 'Y');
}

/* The authoritative external-modification check, run immediately
 * before writing.  Returns 1 to proceed with the save, 0 to abandon
 */
static int preSaveCheck(struct buffer *buf) {
	if (buf->filename == NULL || buf->special_buffer)
		return 1;
	if (buf->open_mtime == 0)
		return 1; /* no baseline: nothing to compare against */

	char *iopath = expandTilde(buf->filename);
	struct stat st;
	int rc = stat(iopath, &st);
	free(iopath);

	int vanished = (rc != 0);
	if (!vanished && st.st_mtime == buf->open_mtime &&
	    st.st_size == buf->open_size)
		return 1; /* unchanged */

	buf->external_mod = 1;

	return confirmYN(
		vanished ?
			"File no longer exists on disk. Save anyway? (y or n)" :
			"File has changed on disk since it was read. Save anyway? (y or n)");
}

/*
 * Save the current buffer.
 *
 * If skip_backup is nonzero, the user has explicitly requested an unsafe
 * save without creating a backup.
 *
 * If skip_backup is zero and the target file already exists, attempt to
 * create a verified backup.  If that fails, ask whether the user wants
 * to continue without a backup.
 */
static void saveBuffer(int skip_backup) {
	char *iopath = NULL;
	char *buf = NULL;
	size_t len = 0;

	char backup_path[PATH_MAX] = { 0 };
	int have_backup = 0;
	int damaged = 0;

	if (E.recording || E.playback) {
		setStatusMessage("Not available during macro");
		return;
	}

	if (!checkUTF8Validity(E.buf)) {
		setStatusMessage("Save failed: buffer contains invalid UTF-8");
		return;
	}

	if (E.buf->filename == NULL || E.buf->special_buffer) {
		char *input = (char *)editorPrompt(
			E.buf, "Save as: ", PROMPT_FILES, NULL);

		if (input == NULL) {
			setStatusMessage("Save aborted.");
			return;
		}

		free(E.buf->filename);
		E.buf->filename = collapseHome(input);
		free(input);

		E.buf->special_buffer = 0;
		E.buf->read_only = 0;

		computeDisplayNames();
	}

	iopath = expandTilde(E.buf->filename);
	if (iopath == NULL) {
		setStatusMessage("Save failed: cannot expand filename");
		return;
	}

	buf = rowsToString(E.buf, &len);
	if (buf == NULL) {
		free(iopath);
		setStatusMessage("Save failed: cannot read buffer");
		return;
	}

	/*
	 * Attempt to create a backup unless the user explicitly requested
	 * an unsafe save.
	 */
	if (!skip_backup) {
		struct stat st;

		if (stat(iopath, &st) == 0 && S_ISREG(st.st_mode)) {
			int backup_rc = make_verified_backup(
				iopath, backup_path, sizeof(backup_path));
			relockAll();

			if (backup_rc == 0) {
				have_backup = 1;
			} else {
				char msg[PATH_MAX + 256];

				msg[0] = '\0';
				emil_strlcat(msg, "Cannot create backup: ",
					     sizeof(msg));
				emil_strlcat(msg, strerror(errno), sizeof(msg));
				emil_strlcat(msg,
					     ". Save without backup? (y/n)",
					     sizeof(msg));

				if (!confirmYN(msg)) {
					setStatusMessage("Save aborted.");
					goto out;
				}

				/*
				 * The user accepted an unsafe save.  No backup
				 * exists for this save operation.
				 */
			}
		}
	}

	/*
	 * Write loop.
	 *
	 * Backup creation is intentionally not repeated here.  If the first
	 * write damages the file, retrying must not create a new backup of
	 * the damaged file.
	 */
	while (1) {
		int rc =
			write_in_place(iopath, buf, len, have_backup, &damaged);
		relockAll();

		if (rc == 0) {
			/*
			 * Success.  Delete only a backup that this save created.
			 */

			if (have_backup)
				unlink(backup_path);
			break;
		}

		char msg[PATH_MAX + 256];

		msg[0] = '\0';

		emil_strlcat(msg, damaged ? "Write" : "Save", sizeof(msg));
		emil_strlcat(msg, " failed: ", sizeof(msg));
		emil_strlcat(msg, strerror(errno), sizeof(msg));

		if (damaged)
			emil_strlcat(msg, ". File contents incomplete or corrupt",
				     sizeof(msg));

		if (have_backup) {
			emil_strlcat(msg, ". Backup is in ", sizeof(msg));
			emil_strlcat(msg, backup_path, sizeof(msg));
		}

		emil_strlcat(msg, ". Retry? (y/n)", sizeof(msg));

		if (!confirmYN(msg)) {
			if (damaged && have_backup) {
				setStatusMessage(
					"Save aborted. File contents may be incomplete or corrupt; backup is in %s",
					backup_path);
			} else if (damaged) {
				setStatusMessage(
					"Save aborted. File may be damaged.");
			} else if (have_backup) {
				setStatusMessage(
					"Save aborted. Backup left in %s",
					backup_path);
			} else {
				setStatusMessage("Save aborted.");
			}

			goto out;
		}
	}

	markBufferClean(E.buf);

	for (int i = 0; i < E.buf->numrows; i++) {
		erow *row = &E.buf->row[i];

		if (row->charcap > row->size + 1) {
			row->chars = xrealloc(row->chars, row->size + 1);
			row->charcap = row->size + 1;
		}
	}

	struct stat save_st;
	if (stat(iopath, &save_st) == 0) {
		E.buf->open_mtime = save_st.st_mtime;
		E.buf->open_size = save_st.st_size;
	}

	E.buf->external_mod = 0;
	E.buf->internal_mod = 1;

	int n = snprintf(NULL, 0, "Wrote %d bytes to %s", (int)len,
			 E.buf->filename);
	char *showName =
		leftTruncate(E.buf->filename, nameFit(E.buf->filename, n));

	setStatusMessage("Wrote %d bytes to %s", (int)len, showName);

	free(showName);

out:
	free(buf);
	free(iopath);
}

/*
 * Normal save command.
 *
 * A nonzero universal argument means: skip backup and perform an unsafe
 * save.
 */
void save(int uarg) {
	int skip_backup = (uarg != 0);

	if (!preSaveCheck(E.buf)) {
		setStatusMessage("Save aborted.");
		return;
	}

	if (E.buf->filename != NULL && !E.buf->special_buffer &&
	    !E.buf->dirty && !E.buf->external_mod) {
		setStatusMessage("(No changes need to be saved)");
		return;
	}

	saveBuffer(skip_backup);
}

/*
 * Save As.
 *
 * This version does not take a universal argument.  If you want Save As to
 * honor the universal argument too, change this to saveAs(int uarg) and call
 * saveBuffer(uarg != 0).
 */
void saveAs(void) {
	if (E.recording || E.playback) {
		setStatusMessage("Not available during macro");
		return;
	}

	char *new_filename =
		(char *)editorPrompt(E.buf, "Save as: ", PROMPT_FILES, NULL);

	if (new_filename == NULL) {
		setStatusMessage("Save aborted.");
		return;
	}

	free(E.buf->filename);
	E.buf->filename = collapseHome(new_filename);
	free(new_filename);

	/*
	 * The buffer now refers to a different file, so release the old
	 * lock and discard the old external-modification baseline.
	 */
	releaseLock(E.buf);

	E.buf->open_mtime = 0;
	E.buf->open_size = 0;
	E.buf->external_mod = 0;
	E.buf->read_only_by_lock = 0;

	E.buf->special_buffer = 0;
	E.buf->read_only = 0;

	computeDisplayNames();

	saveBuffer(0);
}

/* Switch the focused window to the named file.  If a buffer with that
 * filename already exists, reuse it; otherwise open a new one.
 * Returns the buffer on success, NULL on failure. */
struct buffer *switchToFile(const char *filename) {
	/* Check if already open */
	struct buffer *buf = findBufferByName(filename);
	if (buf) {
		E.buf = buf;
		E.windows[windowFocusedIdx()]->buf = buf;
		resetFileCheckThrottle();
		return buf;
	}

	/* Open new buffer */
	struct buffer *nb = newBuffer();
	if (editorOpen(nb, filename) < 0) {
		destroyBuffer(nb);
		return NULL;
	}
	nb->next = E.headbuf;
	E.headbuf = nb;
	E.buf = nb;
	E.windows[windowFocusedIdx()]->buf = nb;
	return nb;
}

/* Check whether a filename contains glob wildcard characters. */
static int hasGlobChars(const char *s) {
	for (; *s; s++) {
		if (*s == '*' || *s == '?' || *s == '[')
			return 1;
	}
	return 0;
}

void findFile(int read_only) {
	/* Not allowed during macro record/playback */
	if (E.recording || E.playback) {
		setStatusMessage("Not available during macro");
		return;
	}

	uint8_t *prompt = editorPrompt(
		E.buf, read_only ? "Find File Read Only: " : "Find File: ",
		PROMPT_FILES, NULL);

	if (prompt == NULL) {
		setStatusMessage("Canceled.");
		return;
	}

	/* If the input contains glob wildcards, expand and open all matches */
	if (hasGlobChars((char *)prompt)) {
		/* Expand ~ for glob: OS doesn't understand tilde */
		char *glob_input = expandTilde((char *)prompt);
		glob_t gl;
		int rc = glob(glob_input, GLOB_MARK, NULL, &gl);
		free(glob_input);
		if (rc != 0 || gl.gl_pathc == 0) {
			if (rc == 0)
				globfree(&gl);
			setStatusMessage("No matching files: %s", prompt);
			free(prompt);
			return;
		}

		struct buffer *last = NULL;
		int opened = 0;
		for (size_t i = 0; i < gl.gl_pathc; i++) {
			/* Skip directories (GLOB_MARK appends '/') */
			size_t plen = strlen(gl.gl_pathv[i]);
			if (plen > 0 && gl.gl_pathv[i][plen - 1] == '/')
				continue;
			struct buffer *buf = switchToFile(gl.gl_pathv[i]);
			if (buf) {
				if (read_only) {
					/* Explicit user request: not
					 * ours to lift when a lock
					 * clears. */
					buf->read_only = 1;
					buf->read_only_by_lock = 0;
					setStatusMessage("Buffer is read-only");
				}
				last = buf;
				opened++;
			}
		}
		globfree(&gl);
		free(prompt);

		if (last) {
			E.buf = last;
			E.windows[windowFocusedIdx()]->buf = last;
			computeDisplayNames();
			refreshScreen();
		}
		if (opened > 1)
			setStatusMessage("Opened %d files", opened);
		return;
	}

	/* Safety net: if a directory path somehow gets through the prompt,
	 * don't try to open it as a file. */
	struct stat st;
	char *stat_path = expandTilde((char *)prompt);
	if (stat(stat_path, &st) == 0 && S_ISDIR(st.st_mode)) {
		setStatusMessage("Directory editing not supported.");
		free(stat_path);
		free(prompt);
		return;
	}
	free(stat_path);

	struct buffer *buf = switchToFile((char *)prompt);
	computeDisplayNames();
	free(prompt);
	if (buf) {
		if (read_only) {
			/* Explicit user request: not ours to lift when a
			 * lock clears. */
			buf->read_only = 1;
			buf->read_only_by_lock = 0;
			setStatusMessage("Buffer is read-only");
		}
		refreshScreen();
	}
}

/* Body of insert-file, callable by tests without going through the
 * minibuffer prompt.  See fileio.h for contract. */
static int insertFileAtPathBody(struct buffer *buf, const char *path,
				const char *display_name);

/* Wrapped for the same reason as editorOpen, and this is the path §F2
 * was reported against: `C-x i` on the file the buffer is already
 * visiting fopen()s and fclose()s it, which released the lock
 * markBufferDirty() had taken on the very buffer doing the inserting. */
int insertFileAtPath(struct buffer *buf, const char *path,
		     const char *display_name) {
	int rc = insertFileAtPathBody(buf, path, display_name);
	relockAll();
	return rc;
}

static int insertFileAtPathBody(struct buffer *buf, const char *path,
				const char *display_name) {
	if (rejectIfReadOnly(buf))
		return 1;

	if (display_name == NULL)
		display_name = path;

	/* Reject directories and check file size against hard limit */
	struct stat ist;
	if (stat(path, &ist) == 0) {
		if (S_ISDIR(ist.st_mode)) {
			setStatusMessage("Directory editing not supported.");
			return 1;
		}
		if (S_ISREG(ist.st_mode) &&
		    (size_t)ist.st_size > EMIL_MAX_FILE_SIZE) {
			setStatusMessage("Exceeds 1 GiB limit");
			return 1;
		}
	}

	FILE *fp = fopen(path, "r");
	if (!fp) {
		if (errno == ENOENT) {
			int n = snprintf(NULL, 0, "File not found: %s",
					 display_name);
			char *showName = leftTruncate(display_name,
						      nameFit(display_name, n));
			setStatusMessage("File not found: %s", showName);
			free(showName);
		} else {
			setStatusMessage("Error opening file: %s",
					 strerror(errno));
		}
		return 1;
	}

	/* Pre-scan for null bytes */
	if (fileContainsNullBytes(fp)) {
		fclose(fp);
		setStatusMessage("File contains null bytes (binary file?)");
		return 1;
	}

	/* Load into a temporary buffer so we can validate before
	 * modifying the real buffer.  Split on newlines exactly as
	 * editorOpen does, so tmpbuf holds the normal representation. */
	struct buffer *tmpbuf = newBuffer();
	bufferResetRows(tmpbuf);

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;
	int ends_with_newline = 0;

	while ((linelen = emil_getline(&line, &linecap, fp)) != -1) {
		ends_with_newline = (linelen > 0 && line[linelen - 1] == '\n');
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
				       line[linelen - 1] == '\r')) {
			linelen--;
		}
		appendRowRaw(tmpbuf, (const uint8_t *)line, linelen);
	}
	if (tmpbuf->numrows == 0 || ends_with_newline)
		appendRowRaw(tmpbuf, (const uint8_t *)"", 0);

	free(line);
	fclose(fp);

	/* Validate UTF-8 before inserting */
	if (!checkUTF8Validity(tmpbuf)) {
		destroyBuffer(tmpbuf);
		setStatusMessage("Failed UTF-8 validation");
		return 1;
	}

	int lines_inserted = bufferLineCount(tmpbuf);

	if (lines_inserted > 0) {
		/* C-x i inserts whole lines: the file's last line stays
			 * a line of its own rather than merging with the text
			 * at point, and the row already at point keeps its
			 * content on a row below the insertion.  So the byte
			 * block always ends in a newline, whether or not the
			 * file did.  Insert position is (0, buf->cy), the start
			 * of the current row. */
		int saved_cy = buf->cy;

		size_t rawlen = 0;
		char *raw = rowsToString(tmpbuf, &rawlen);
		struct dbuf d = DBUF_INIT;
		dbuf_append(&d, (const uint8_t *)raw, (int)rawlen);
		if (rawlen == 0 || raw[rawlen - 1] != '\n')
			dbuf_byte(&d, '\n');
		free(raw);

		int byte_len;
		uint8_t *bytes = dbuf_detach(&d, &byte_len);

		int ex, ey;
		mutateInsert(buf, 0, saved_cy, bytes, byte_len, &ex, &ey);
		free(bytes);

		(void)ex;
		(void)ey;
		buf->cy = saved_cy + lines_inserted - 1;
		buf->cx = buf->row[buf->cy].size;
	}

	destroyBuffer(tmpbuf);

	int n = snprintf(NULL, 0, "Inserted %d lines from %s", lines_inserted,
			 display_name);
	char *showName = leftTruncate(display_name, nameFit(display_name, n));
	setStatusMessage("Inserted %d lines from %s", lines_inserted, showName);
	free(showName);

	return 0;
}

void insertFile(void) {
	struct buffer *buf = E.buf;

	/* Refuse before prompting for a filename.  insertFileAtPath
	 * remains the load-bearing check; this one only spares the
	 * user typing a path for an insertion that will be refused. */
	if (rejectIfReadOnly(buf))
		return;

	uint8_t *filename =
		editorPrompt(buf, "Insert file: ", PROMPT_FILES, NULL);
	if (filename == NULL) {
		return;
	}

	char *iopath = expandTilde((char *)filename);
	(void)insertFileAtPath(buf, iopath, (const char *)filename);
	free(iopath);
	free(filename);
}

/* Compute the relative path from directory 'from' to directory 'to'.
 * Both must be absolute paths.  Returns a malloc'd string.
 * Example: from="/a/b/c", to="/a/d" => "../../d" */
char *relativePath(const char *from, const char *to) {
	/* Find the common prefix, breaking on '/' boundaries.
	 * split will point just past the last shared '/' separator,
	 * so from[split..] and to[split..] are the diverging tails. */
	int split = 0;
	int i = 0;
	while (from[i] && to[i] && from[i] == to[i]) {
		if (from[i] == '/')
			split = i + 1;
		i++;
	}
	/* Handle one being a prefix of the other:
	 * e.g. from="/a/b/c" to="/a/b"  (to ends, from continues with '/')
	 *      from="/a/b"   to="/a/b/c" (from ends, to continues with '/')
	 *      from="/a/b"   to="/a/b"   (both end) */
	if (from[i] == '\0' && to[i] == '\0')
		split = i;
	else if (from[i] == '\0' && to[i] == '/')
		split = i;
	else if (to[i] == '\0' && from[i] == '/')
		split = i + 1;

	/* Count directory segments remaining in 'from' after split */
	int ups = 0;
	for (int j = split; from[j]; j++) {
		if (from[j] == '/' && from[j + 1] != '\0')
			ups++;
	}
	if (from[split] != '\0')
		ups++;

	/* Tail of 'to' after split: careful not to read past end */
	const char *to_tail = "";
	if ((int)strlen(to) > split) {
		to_tail = to + split;
		if (*to_tail == '/')
			to_tail++;
	}

	int tail_len = strlen(to_tail);
	int result_len = ups * 3 + tail_len + 1;
	char *result = xmalloc(result_len);
	result[0] = '\0';

	for (int j = 0; j < ups; j++)
		emil_strlcat(result, "../", result_len);

	if (tail_len > 0)
		emil_strlcat(result, to_tail, result_len);
	else if (ups > 0)
		result[strlen(result) - 1] = '\0'; /* trim trailing / */

	return result;
}

/* Canonicalize an absolute path by resolving . and .. segments.
 * Does NOT resolve symlinks: purely string-level.
 * Modifies the string in place and returns it. */
char *cleanPath(char *path) {
	/* Stack of pointers to segment starts within path.
	 * PATH_MAX/2 is the theoretical maximum number of segments
	 * ("/" plus one-char names), but in practice 256 is generous.
	 * If a path somehow exceeds this, return it unmodified rather
	 * than silently dropping segments. */
	char *segs[256];
	int depth = 0;
	int overflow = 0;

	char *p = path;
	if (*p == '/')
		p++;

	while (*p) {
		char *seg = p;
		while (*p && *p != '/')
			p++;
		int len = p - seg;
		if (*p == '/')
			p++;

		if (len == 0) {
			/* Empty segment, from a doubled or trailing slash.
			 * absolutePath builds cwd + "/" + path, so at cwd
			 * "/" every relative name arrived as "//name" and
			 * kept a leading empty segment -- "foo" and "/foo"
			 * then resolved to different strings and
			 * findBufferByName opened the same file twice. */
			continue;
		} else if (len == 1 && seg[0] == '.') {
			continue;
		} else if (len == 2 && seg[0] == '.' && seg[1] == '.') {
			if (depth > 0)
				depth--;
		} else {
			if (depth >= 256) {
				overflow = 1;
				break;
			}
			segs[depth++] = seg;
			/* null-terminate this segment for later copy */
			seg[len] = '\0';
		}
	}

	if (overflow)
		return path; /* too many segments, return unmodified */

	/* Reassemble */
	char *out = path;
	*out++ = '/';
	for (int i = 0; i < depth; i++) {
		int slen = strlen(segs[i]);
		memmove(out, segs[i], slen);
		out += slen;
		if (i < depth - 1)
			*out++ = '/';
	}
	*out = '\0';
	return path;
}

/* Resolve a path to absolute form for comparison purposes.
 * Normalizes . and .. segments.  Does NOT resolve symlinks.
 * Returns a new string; caller frees. */
char *absolutePath(const char *path) {
	if (!path || !*path)
		return xstrdup("");

	if (path[0] == '/') {
		char *out = xstrdup(path);
		cleanPath(out);
		return out;
	}

	if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
		char *out = expandTilde(path);
		cleanPath(out);
		return out;
	}

	char cwd[PATH_MAX];
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return xstrdup(path);

	size_t clen = strlen(cwd);
	size_t plen = strlen(path);
	char *out = xmalloc(clen + 1 + plen + 1);
	memcpy(out, cwd, clen);
	out[clen] = '/';
	memcpy(out + clen + 1, path, plen + 1);
	cleanPath(out);
	return out;
}

/* Rebase a relative filename from old_cwd to new_cwd.
 * Returns a new malloc'd string.  Absolute paths are returned as-is (duped).
 * Used by changeDirectory and exposed for testing. */
char *rebaseFilename(const char *filename, const char *old_cwd,
		     const char *new_cwd) {
	/* Absolute and ~-prefixed paths are location-independent */
	if (filename[0] == '/')
		return xstrdup(filename);
	if (filename[0] == '~' && (filename[1] == '\0' || filename[1] == '/'))
		return xstrdup(filename);

	/* Absolutize against old cwd and clean up any .. segments */
	int abs_len = strlen(old_cwd) + 1 + strlen(filename) + 1;
	char *abs = xmalloc(abs_len);
	snprintf(abs, abs_len, "%s/%s", old_cwd, filename);
	cleanPath(abs);

	/* Relativize the directory part against new cwd,
	 * then reattach the basename */
	char *slash = strrchr(abs, '/');
	char *base = xstrdup(slash + 1);
	*slash = '\0'; /* abs is now the directory */
	char *reldir = relativePath(new_cwd, abs);
	int new_len = strlen(reldir) + 1 + strlen(base) + 1;
	char *new_name = xmalloc(new_len);
	if (reldir[0] == '\0')
		snprintf(new_name, new_len, "%s", base);
	else
		snprintf(new_name, new_len, "%s/%s", reldir, base);

	free(abs);
	free(base);
	free(reldir);
	return new_name;
}

void changeDirectory(void) {
	uint8_t *dir = editorPrompt(E.buf, "Directory: ", PROMPT_DIR, NULL);
	if (dir == NULL) {
		setStatusMessage("Canceled.");
		return;
	}

	/* Grab the old cwd before changing */
	char old_cwd[PATH_MAX];
	if (getcwd(old_cwd, sizeof(old_cwd)) == NULL) {
		setStatusMessage("cd: cannot determine current directory");
		free(dir);
		return;
	}

	char *iodir = expandTilde((char *)dir);
	if (chdir(iodir) != 0) {
		setStatusMessage("cd: %s: %s", (char *)dir, strerror(errno));
		free(iodir);
		free(dir);
		return;
	}
	free(iodir);

	char new_cwd[PATH_MAX];
	if (getcwd(new_cwd, sizeof(new_cwd)) == NULL) {
		/* chdir succeeded but getcwd failed: unlikely but
		 * leave filenames as-is */
		setStatusMessage("Changed directory");
		free(dir);
		return;
	}

	/* If the directory actually changed, update relative filenames */
	if (strcmp(old_cwd, new_cwd) != 0) {
		for (struct buffer *b = E.headbuf; b != NULL; b = b->next) {
			if (b->filename == NULL || b->special_buffer)
				continue;
			char *new_name =
				rebaseFilename(b->filename, old_cwd, new_cwd);
			free(b->filename);
			b->filename = new_name;
		}

		computeDisplayNames();
	}

	setStatusMessage("Current directory: %s", new_cwd);
	free(dir);
}

/*** stdin loading ***/

/*
 * Read all available data from a file descriptor into a malloc'd buffer.
 * Sets *out_len to the number of bytes read.  Returns NULL on allocation
 * failure; returns an empty buffer (out_len == 0) if nothing was read.
 */
char *readAllFromFd(int fd, size_t *out_len) {
	size_t cap = BUFSIZ;
	size_t len = 0;
	char *buf = xmalloc(cap);
	for (;;) {
		ssize_t n = read(fd, buf + len, cap - len);
		if (n > 0) {
			len += (size_t)n;
			if (len >= cap) {
				cap <<= 1;
				buf = xrealloc(buf, cap);
			}
			continue;
		}
		/* A signal (SIGWINCH, SIGCONT, ...) landing mid-read
		 * must not silently truncate the input. */
		if (n < 0 && errno == EINTR)
			continue;
		break; /* EOF, or a real error: return what we have */
	}
	*out_len = len;
	return buf;
}

/*
 * Load piped stdin data into a new editor buffer.  The data is split
 * on newline boundaries and inserted row by row, matching the same
 * approach used by editorOpen().  The buffer is named "*stdin*" and
 * marked read-only.
 *
 * Returns the new buffer, or NULL if the data contains null bytes
 * or is not valid UTF-8.  editorOpen() enforces the same invariant
 * for files; every load path must, because row primitives (see
 * rowDelChar) assume all buffer content is valid UTF-8.
 */
struct buffer *loadStdinBuffer(const char *data, size_t len) {
	/* Reject binary data: null bytes can't be represented */
	if (memchr(data, '\0', len) != NULL) {
		return NULL;
	}

	struct buffer *buf = newBuffer();
	buf->filename = xstrdup("*stdin*");
	bufferResetRows(buf); /* rebuilt from scratch below */

	size_t start = 0;
	for (size_t i = 0; i < len; i++) {
		if (data[i] == '\n') {
			/* Strip trailing \r for DOS line endings */
			size_t end = i;
			if (end > start && data[end - 1] == '\r')
				end--;
			appendRowRaw(buf, (const uint8_t *)&data[start],
				     (int)(end - start));
			start = i + 1;
		}
	}
	/* Handle a final line with no trailing newline. */
	if (start < len) {
		size_t end = len;
		if (end > start && data[end - 1] == '\r')
			end--;
		appendRowRaw(buf, (const uint8_t *)&data[start],
			     (int)(end - start));
	}

	/* Terminate unconditionally, as editorOpen does: piped input
	 * without a final newline would otherwise leave the last row
	 * non-empty, breaking the invariant in buffer.h. */
	appendRowRaw(buf, (const uint8_t *)"", 0);

	/* Validate UTF-8 encoding, mirroring editorOpen: the null-byte
	 * check above only catches a subset of binary input. */
	if (!checkUTF8Validity(buf)) {
		destroyBuffer(buf);
		return NULL;
	}

	/* Stdin content is read-only: the *stdin* pseudo-file has no
	 * disk backing to save to. 
	 */
	buf->read_only = 1;
	buf->word_wrap = 1;
	return buf;
}
