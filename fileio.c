#include "fileio.h"
#include "buffer.h"
#include "dbuf.h"
#include "display.h"
#include "emil.h"
#include "keymap.h"
#include "message.h"
#include "mutate.h"
#include "prompt.h"
#include "terminal.h"
#include "undo.h"
#include "unicode.h"
#include "unused.h"
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
extern struct config E;

/* External functions we need */
extern void die(const char *s);

/*** timed syscall support ***/

/* SIGALRM handler for timed_stat / timed_lockFile.  Interrupts any
 * blocking syscall (stat, open, fcntl) so checkFileModified never
 * stalls the editor on a slow or hung filesystem. */

static volatile sig_atomic_t file_check_timed_out;

static void fileCheckAlarm(int sig) {
	(void)sig;
	file_check_timed_out = 1;
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
	file_check_timed_out = 0;
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

/* Probe whether an advisory lock is held on a file without acquiring one.
 * Returns 0 if no lock is held, PID if locked , -1 if unknown
 * or -2 on error (file doesn't exist, can't open, etc.). */
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

/* Try to acquire an advisory write lock on a file.
 * Returns 0 on success (lock acquired), -1 if already locked (sets
 * status message with the blocking PID), or -2 on error.
 * On success, bufr->lock_fd is set and must be released later. */
int lockFile(struct buffer *bufr, const char *filename) {
	/* Try O_RDWR first (needed for F_WRLCK per POSIX).
	 * Fall back to O_RDONLY + F_RDLCK if the file isn't writable. */
	int fd = open(filename, O_RDWR);
	int use_rdlck = 0;
	if (fd < 0) {
		if (errno == ENOENT)
			return -2; /* file doesn't exist yet: nothing to lock */
		fd = open(filename, O_RDONLY);
		if (fd < 0)
			return -2;
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

		/* Record mtime for external modification detection */
		struct stat st;
		if (fstat(fd, &st) == 0)
			bufr->open_mtime = st.st_mtime;

		return 0;
	}

	/* Lock failed: find out who holds it, record on the buffer for
	 * the persistent status-bar warning. */
	if (errno == EACCES || errno == EAGAIN) {
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
	}

	close(fd);
	return -1;
}

/* Release the advisory lock held by this buffer.
 *
 * Does NOT clear external_mod: that flag is a latch indicating
 * "disk content has diverged from what we loaded," and its only
 * legitimate exits are save (user chose to clobber) and revert
 * (user chose the disk copy).  Releasing a lock: which happens on
 * clean→dirty transitions via markBufferClean, on saveAs, and on
 * buffer destruction: says nothing about whether the buffer still
 * reflects disk.  In particular, undo-to-clean triggers
 * markBufferClean → releaseLock, and clearing external_mod there
 * would silently dismiss a warning the user hasn't resolved. */

void releaseLock(struct buffer *bufr) {
	if (bufr->lock_fd >= 0) {
		close(bufr->lock_fd);
		bufr->lock_fd = -1;
	}
	bufr->open_mtime = 0;
	bufr->lock_blocked_pid = 0;
}

/* Check whether the underlying file has been modified externally,
 * and opportunistically clear a stale lock_blocked_pid warning if
 * the blocking process has since released the lock.
 *
 * Called periodically (from refreshScreen) on the focused buffer.
 *
 * Throttled: at most one check every FILE_CHECK_INTERVAL_SEC seconds
 * (monotonic clock) to avoid hammering slow network filesystems.
 *
 * Timeout-guarded: each stat() / lockFile() call is wrapped in a
 * 50ms SIGALRM deadline so a hung filesystem never stalls the editor.
 *
 * Two independent jobs:
 *
 *   1. Set bufr->external_mod if mtime has drifted since open/save.
 *      One-shot: skipped if the flag is already set.
 *
 *   2. Re-probe the advisory lock when we know we have a stale
 *      warning to clear: the buffer is dirty, we previously tried
 *      and failed to acquire the lock (lock_blocked_pid != 0), and
 *      we still don't hold it (lock_fd < 0).
 *
 *      Job 2 is gated on !external_mod: if the blocking process
 *      saved changes before releasing the lock, Job 1 will have
 *      already set external_mod from the mtime drift, and we
 *      deliberately don't acquire a lock we'd use to overwrite
 *      those changes. */

void checkFileModified(void) {
	if (E.buf->filename == NULL)
		return;

	/* Throttle: skip if we checked recently. */
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0) {
		long elapsed = now.tv_sec - E.last_file_check.tv_sec;
		if (elapsed >= 0 && elapsed < FILE_CHECK_INTERVAL_SEC)
			return;
		E.last_file_check = now;
	}

	/* Job 1: mtime check. */
	if (E.buf->open_mtime != 0 && !E.buf->external_mod) {
		char *iopath = expandTilde(E.buf->filename);
		struct stat st;
		armTimer();
		int rc = stat(iopath, &st);
		disarmTimer();
		if (rc == 0 && !file_check_timed_out) {
			if (st.st_mtime != E.buf->open_mtime) {
				E.buf->external_mod = 1;
				setStatusMessage(msg_warn_file_changed,
						 E.buf->filename);
			}
		}
		free(iopath);
	}

	/* Job 2: stale-lock re-probe. */
	if (E.buf->dirty && E.buf->lock_blocked_pid != 0 &&
	    E.buf->lock_fd < 0 && !E.buf->external_mod) {
		char *iopath = expandTilde(E.buf->filename);
		armTimer();
		int rc = lockFile(E.buf, iopath);
		disarmTimer();
		if (rc == 0) {
			/* Acquired: warning clears.  Do NOT gate this
			 * on !file_check_timed_out: on a slow filesystem
			 * every syscall inside lockFile can succeed
			 * individually while their sum exceeds the 50ms
			 * deadline.  lock_fd is held either way, and
			 * since this job only runs while lock_fd < 0,
			 * skipping the clear here would latch the
			 * contradictory state "lock held + blocked by
			 * PID" (and its status-bar warning) for the
			 * rest of the session.  The timeout flag only
			 * says the probe was slow, not that its result
			 * is invalid: an interrupted syscall makes
			 * lockFile fail, landing in the rc != 0 path. */
			E.buf->lock_blocked_pid = 0;
			setStatusMessage(msg_warn_lock_acquired);
		}
		/* On failure lockFile has already refreshed
		 * lock_blocked_pid to reflect the current holder
		 * (possibly a different process from before). */
		free(iopath);
	}
}

/*** file i/o ***/

char *rowsToString(struct buffer *bufr, size_t *buflen) {
	size_t totlen = 0;
	int j;
	for (j = 0; j < bufr->numrows; j++) {
		totlen += bufr->row[j].size + 1;
	}
	*buflen = totlen;

	char *buf = xmalloc(totlen);
	char *p = buf;
	for (j = 0; j < bufr->numrows; j++) {
		memcpy(p, bufr->row[j].chars, bufr->row[j].size);
		p += bufr->row[j].size;
		*p = '\n';
		p++;
	}

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

int editorOpen(struct buffer *bufr, char *filename) {
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
				setStatusMessage(msg_cant_open,
						 strerror(EISDIR));
				free(bufr->filename);
				bufr->filename = NULL;
				free(iopath);
				return -1;
			}
			setStatusMessage(msg_new_file, bufr->filename);
			free(iopath);
			return 0;
		}
		setStatusMessage(msg_cant_open, strerror(errno));
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
				setStatusMessage(msg_cant_open,
						 strerror(EISDIR));
				free(bufr->filename);
				bufr->filename = NULL;
				free(iopath);
				return -1;
			}
			if (S_ISREG(st.st_mode) &&
			    (size_t)st.st_size > EMIL_MAX_FILE_SIZE) {
				fclose(fp);
				setStatusMessage(msg_memory_limit);
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
		setStatusMessage(msg_binary_file);
		free(bufr->filename);
		bufr->filename = NULL;
		free(iopath);
		return -1;
	}

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while ((linelen = emil_getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 &&
		       (line[linelen - 1] == '\n' || line[linelen - 1] == '\r'))
			linelen--;
		appendRowRaw(bufr, (const uint8_t *)line, linelen);
	}

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
		for (int i = 0; i < bufr->numrows; i++)
			freeRow(&bufr->row[i]);
		free(bufr->row);
		bufr->row = NULL;
		bufr->numrows = 0;
		bufr->rowcap = 0;
		free(bufr->filename);
		bufr->filename = NULL;
		setStatusMessage("File has too many lines");
		free(iopath);
		return -1;
	}

	/* Validate UTF-8 encoding of the loaded content */
	if (!checkUTF8Validity(bufr)) {
		for (int i = 0; i < bufr->numrows; i++) {
			freeRow(&bufr->row[i]);
		}
		free(bufr->row);
		bufr->row = NULL;
		bufr->numrows = 0;
		bufr->rowcap = 0;
		free(bufr->filename);
		bufr->filename = NULL;
		setStatusMessage(msg_invalid_utf8);
		free(iopath);
		return -1;
	}

	/* The load used appendRowRaw which deliberately does not dirty
	 * the buffer or invalidate per-row; invalidate the screen cache
	 * once here now that all rows are in place.  The buffer is
	 * already clean (newBuffer initialized it that way, and the
	 * load did not touch dirty state), so no markBufferClean is
	 * needed. */
	invalidateScreenCache(bufr);

	if (access(iopath, W_OK) != 0) {
		bufr->read_only = 1;
	}

	/* Record mtime for external-modification detection.  The lock
	 * used to be acquired here as a side effect; under the "lock only
	 * while dirty" policy (issue #49), the lock is deferred until the
	 * buffer is actually modified: see markBufferDirty(). */
	{
		struct stat st;
		if (stat(iopath, &st) == 0)
			bufr->open_mtime = st.st_mtime;
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

	if (lock_pid != 0) {
		bufr->read_only = 1;
		bufr->lock_blocked_pid = lock_pid;
		if (lock_pid > 0)
			setStatusMessage(msg_read_only_locked, lock_pid);
		else
			setStatusMessage(msg_read_only_locked_unknown);
	} else {
		setStatusMessage(msg_lines_columns, bufr->numrows, max_width);
	}
	return 0;
}

void revert(void) {
	struct buffer *buf = E.buf;
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
	new->indent = buf->indent;
	new->cx = buf->cx;
	new->cy = buf->cy;
	if (new->numrows == 0) {
		new->cy = 0;
		new->cx = 0;
	} else if (new->cy >= new->numrows) {
		new->cy = new->numrows - 1;
		new->cx = 0;
	} else if (new->cx > new->row[new->cy].size) {
		new->cx = new->row[new->cy].size;
	}
	destroyBuffer(buf);
}

static int writeAtomic(const char *iopath, const char *buf, size_t len) {
	/* Follow symlinks: if iopath is a symlink, we want to replace
	 * the file it points at, not clobber the link with a regular
	 * file (which would orphan e.g. a dotfile managed in a repo).
	 * realpath resolves the link; on a new file it fails with
	 * ENOENT and we keep the original path. */
	char resolved[PATH_MAX];
	const char *target = iopath;
	if (realpath(iopath, resolved) != NULL)
		target = resolved;

	char tmpname[PATH_MAX];
	if ((size_t)snprintf(tmpname, sizeof(tmpname), "%s.tmpXXXXXX",
			     target) >= sizeof(tmpname)) {
		errno = ENAMETOOLONG;
		return -1;
	}

	int fd = mkstemp(tmpname);
	if (fd == -1)
		return -1;

	/* Preserve the existing file's permissions; for a new file,
	 * use 0644 masked by the umask (mkstemp creates 0600) to
	 * match writeDirect's open(..., 0644) semantics. */
	struct stat st;
	if (stat(target, &st) == 0) {
		fchmod(fd, st.st_mode);
	} else {
		mode_t um = umask(0);
		umask(um);
		fchmod(fd, 0644 & ~um);
	}

	size_t total = 0;
	while (total < len) {
		ssize_t n = write(fd, buf + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			unlink(tmpname);
			return -1;
		}
		if (n == 0) {
			close(fd);
			unlink(tmpname);
			errno = EIO;
			return -1;
		}
		total += (size_t)n;
	}

	if (fsync(fd) == -1) {
		close(fd);
		unlink(tmpname);
		return -1;
	}

	if (close(fd) == -1) {
		unlink(tmpname);
		return -1;
	}

	if (rename(tmpname, target) == -1) {
		unlink(tmpname);
		return -1;
	}

	/* The fsync above got the file's contents onto stable storage;
	 * rename() only altered the parent directory, whose entry is
	 * still just dirty page cache.  Without this second sync a power
	 * loss can lose the rename and leave the old file in place, so
	 * the save silently reverts.  Errors are deliberately ignored:
	 * the data is already durable and the rename has succeeded, so
	 * the file is intact either way, and some filesystems reject
	 * fsync on a directory fd with EINVAL.  Reporting failure here
	 * would tell the user the save failed when it did not. */
	char dirpath[PATH_MAX];
	const char *slash = strrchr(target, '/');
	if (slash == NULL) {
		dirpath[0] = '.';
		dirpath[1] = '\0';
	} else {
		size_t dlen = (slash == target) ? 1 : (size_t)(slash - target);
		memcpy(dirpath, target, dlen);
		dirpath[dlen] = '\0';
	}

	int dfd = open(dirpath, O_RDONLY);
	if (dfd != -1) {
		fsync(dfd);
		close(dfd);
	}

	return 0;
}

static int writeDirect(const char *iopath, const char *buf, size_t len) {
	int fd = open(iopath, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd == -1)
		return -1;

	size_t total = 0;
	while (total < len) {
		ssize_t n = write(fd, buf + total, len - total);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			close(fd);
			return -1;
		}
		if (n == 0) {
			close(fd);
			errno = EIO;
			return -1;
		}
		total += (size_t)n;
	}

	if (fsync(fd) == -1) {
		close(fd);
		return -1;
	}

	return close(fd);
}

static int confirmOverwriteDirect(const char *msg) {
	/* Suppress checkFileModified's "[FILE CHANGED ON DISK]" during
	 * the prompt: refreshScreen() calls checkFileModified(), and a
	 * failing writeAtomic() may have touched the file's mtime before
	 * rolling back, which would print the stale-file warning on top
	 * of our y/N prompt.  checkFileModified() no-ops when
	 * open_mtime == 0, so zero it for the duration and restore
	 * after. */
	time_t saved_mtime = E.buf->open_mtime;
	E.buf->open_mtime = 0;

	setStatusMessage("%s", msg);
	refreshScreen();

	int c = readKey();

	clearStatusMessage(); /* clear prompt */

	E.buf->open_mtime = saved_mtime;

	return (c == 'y' || c == 'Y');
}

void save(void) {
	if (E.recording || E.playback) {
		setStatusMessage(msg_macro_blocked);
		return;
	}

	if (E.buf->filename == NULL) {
		char *input = (char *)editorPrompt(
			E.buf, "Save as: ", PROMPT_FILES, NULL);
		if (input == NULL) {
			setStatusMessage(msg_save_aborted);
			return;
		}
		E.buf->filename = collapseHome(input);
		free(input);
		E.buf->special_buffer = 0;
		computeDisplayNames();
	}

	char *iopath = expandTilde(E.buf->filename);

	size_t len;
	char *buf = rowsToString(E.buf, &len);

	/* Try atomic write first */
	if (writeAtomic(iopath, buf, len) == -1) {
		if (errno == ENOSPC) {
			if (!confirmOverwriteDirect(msg_save_directly_prompt)) {
				free(buf);
				free(iopath);
				setStatusMessage(msg_save_aborted);
				return;
			}

			if (writeDirect(iopath, buf, len) == -1) {
				free(buf);
				free(iopath);
				setStatusMessage(msg_save_failed,
						 strerror(errno));
				return;
			}

		} else {
			free(buf);
			free(iopath);
			setStatusMessage(msg_save_failed, strerror(errno));
			return;
		}
	}
	/* Success  */

	free(buf);
	markBufferClean(E.buf);

	for (int i = 0; i < E.buf->numrows; i++) {
		erow *row = &E.buf->row[i];
		if (row->charcap > row->size + 1) {
			row->chars = xrealloc(row->chars, row->size + 1);
			row->charcap = row->size + 1;
		}
	}

	struct stat save_st;
	if (stat(iopath, &save_st) == 0)
		E.buf->open_mtime = save_st.st_mtime;

	E.buf->external_mod = 0;
	E.buf->internal_mod = 1;

	/* Lock is released on clean by markBufferClean above; reacquired
	 * on the next clean→dirty transition. */

	int n = snprintf(NULL, 0, msg_wrote_bytes, (int)len, E.buf->filename);
	char *showName =
		leftTruncate(E.buf->filename, nameFit(E.buf->filename, n));
	setStatusMessage(msg_wrote_bytes, (int)len, showName);
	free(showName);

	free(iopath);
}

void saveAs(void) {
	/* Not allowed during macro record/playback */
	if (E.recording || E.playback) {
		setStatusMessage(msg_macro_blocked);
		return;
	}
	char *new_filename =
		(char *)editorPrompt(E.buf, "Save as: ", PROMPT_FILES, NULL);
	if (new_filename == NULL) {
		setStatusMessage(msg_save_aborted);
		return;
	}
	free(E.buf->filename);
	E.buf->filename = collapseHome(new_filename);
	free(new_filename);
	/* Release lock on the old file before saving to the new one */
	releaseLock(E.buf);
	computeDisplayNames();
	save();
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
	if (editorOpen(nb, (char *)filename) < 0) {
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
		setStatusMessage(msg_macro_blocked);
		return;
	}

	uint8_t *prompt = editorPrompt(
		E.buf, read_only ? msg_find_file_read_only : msg_find_file,
		PROMPT_FILES, NULL);

	if (prompt == NULL) {
		setStatusMessage(msg_canceled);
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
			setStatusMessage(msg_no_glob_match, prompt);
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
					buf->read_only = 1;
					setStatusMessage(msg_read_only);
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
	 * don't try to open it as a file. * TODO review with suspicion */
	struct stat st;
	char *stat_path = expandTilde((char *)prompt);
	if (stat(stat_path, &st) == 0 && S_ISDIR(st.st_mode)) {
		setStatusMessage(msg_dir_not_supported);
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
			buf->read_only = 1;
			setStatusMessage(msg_read_only);
		}
		refreshScreen();
	}
}

/* Body of insert-file, callable by tests without going through the
 * minibuffer prompt.  See fileio.h for contract. */
int insertFileAtPath(struct buffer *buf, const char *path,
		     const char *display_name) {
	if (rejectIfReadOnly(buf))
		return 1;

	if (display_name == NULL)
		display_name = path;

	/* Reject directories and check file size against hard limit */
	struct stat ist;
	if (stat(path, &ist) == 0) {
		if (S_ISDIR(ist.st_mode)) {
			setStatusMessage(msg_dir_not_supported);
			return 1;
		}
		if (S_ISREG(ist.st_mode) &&
		    (size_t)ist.st_size > EMIL_MAX_FILE_SIZE) {
			setStatusMessage(msg_memory_limit);
			return 1;
		}
	}

	FILE *fp = fopen(path, "r");
	if (!fp) {
		if (errno == ENOENT) {
			int n = snprintf(NULL, 0, msg_file_not_found,
					 display_name);
			char *showName =
				leftTruncate((char *)display_name,
					     nameFit((char *)display_name, n));
			setStatusMessage(msg_file_not_found, showName);
			free(showName);
		} else {
			setStatusMessage(msg_error_opening, strerror(errno));
		}
		return 1;
	}

	/* Pre-scan for null bytes */
	if (fileContainsNullBytes(fp)) {
		fclose(fp);
		setStatusMessage(msg_binary_file);
		return 1;
	}

	/* Load into a temporary buffer so we can validate before
	 * modifying the real buffer */
	struct buffer *tmpbuf = newBuffer();

	char *line = NULL;
	size_t linecap = 0;
	ssize_t linelen;

	while ((linelen = emil_getline(&line, &linecap, fp)) != -1) {
		while (linelen > 0 && (line[linelen - 1] == '\n' ||
				       line[linelen - 1] == '\r')) {
			linelen--;
		}
		appendRowRaw(tmpbuf, (const uint8_t *)line, linelen);
	}

	free(line);
	fclose(fp);

	/* Validate UTF-8 before inserting */
	if (!checkUTF8Validity(tmpbuf)) {
		destroyBuffer(tmpbuf);
		setStatusMessage(msg_invalid_utf8);
		return 1;
	}

	int lines_inserted = tmpbuf->numrows;

	if (lines_inserted > 0) {
		/* Concatenate the validated rows into a single byte block
		 * separated by newlines.
		 *
		 * Trailing newline policy:
		 *   - If buf->cy addresses an existing row, we want that row's
		 *     content to remain a separate row after the inserted
		 *     block.  Adding a trailing '\n' causes
		 *     bulkInsert's "save suffix / insert last fragment / emit
		 *     suffix as new row" path to leave the existing content on
		 *     its own row below the insertion.
		 *   - If buf->cy == buf->numrows (past-end virtual row, empty
		 *     buffer case), there's no suffix to preserve and a
		 *     trailing '\n' would manufacture an extra empty row that
		 *     wasn't there before, so omit it.
		 *
		 * Insert position is (0, buf->cy): start of the current row:
		 * in both cases. */
		int saved_cy = buf->cy;
		int has_suffix_row = (saved_cy < buf->numrows);

		struct dbuf d = DBUF_INIT;
		for (int i = 0; i < tmpbuf->numrows; i++) {
			if (i > 0)
				dbuf_byte(&d, '\n');
			dbuf_append(&d, tmpbuf->row[i].chars,
				    tmpbuf->row[i].size);
		}
		if (has_suffix_row)
			dbuf_byte(&d, '\n');

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

	int n = snprintf(NULL, 0, msg_inserted_lines, lines_inserted,
			 display_name);
	char *showName = leftTruncate((char *)display_name,
				      nameFit((char *)display_name, n));
	setStatusMessage(msg_inserted_lines, lines_inserted, showName);
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

		if (len == 1 && seg[0] == '.') {
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
			if (seg[len] != '\0')
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
		setStatusMessage(msg_canceled);
		return;
	}

	/* Grab the old cwd before changing */
	char old_cwd[PATH_MAX];
	if (getcwd(old_cwd, sizeof(old_cwd)) == NULL) {
		setStatusMessage(msg_indeterminate_cd);
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
		setStatusMessage(msg_changed_dir);
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

	setStatusMessage(msg_current_dir, new_cwd);
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
	/* Handle final line without trailing newline */
	if (start < len) {
		size_t end = len;
		if (end > start && data[end - 1] == '\r')
			end--;
		appendRowRaw(buf, (const uint8_t *)&data[start],
			     (int)(end - start));
	}

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
	invalidateScreenCache(buf);
	buf->word_wrap = 1;
	return buf;
}
