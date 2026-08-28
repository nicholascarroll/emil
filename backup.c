/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include "backup.h"
#include "unicode.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <libgen.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/*** Backup and Write Strategy (Vim backupcopy=yes style) ***/

static int createBackupExclusive(const char *name) {
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

	fd = createBackupExclusive(backup);
	if (fd != -1)
		return fd;

	if (errno != EEXIST)
		return -1;

	char_start = len;
	if (char_start > 0)
		char_start--;
	while (char_start > 0 && utf8_isCont((uint8_t)path[char_start]))
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

		fd = createBackupExclusive(backup);
		if (fd != -1)
			return fd;

		if (errno != EEXIST)
			return -1;
	}

	errno = EEXIST;
	return -1;
}

/*
 * Copy all bytes from from_fd to to_fd.
 */
static int copyFd(int from_fd, int to_fd) {
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
 * Attempt to fsync the parent directory of the given filepath.
 * This ensures that the directory entry for a newly created file
 * is persisted to disk on filesystems that require it.
 *
 * Returns 0 on success or if the operation is unsupported/harmless.
 * Returns -1 on genuine I/O or storage failure.
 */
static int syncParentDirectory(const char *filepath) {
	if (!filepath)
		return 0; /* Invalid file path was passed in 🙈. Don't blame the filesystem.*/

	char *path_copy = xstrdup(filepath);
	if (!path_copy)
		return -1;

	char *dir_path = dirname(path_copy);
	int dir_fd = open(dir_path, O_RDONLY
#ifdef O_DIRECTORY
					    | O_DIRECTORY
#endif
	);
	free(path_copy);

	if (dir_fd < 0) {
		/*
		 * Cannot open directory (e.g. EACCES on a write-only
		 * drop-box directory). Treat as unsupported/fallback
		 * and proceed without directory sync.
		 */
		return 0;
	}

	int sync_result;
	do {
		sync_result = fsync(dir_fd);
	} while (sync_result == -1 &&
		 errno == EINTR); /* Retry if interrupted by signal */

	int saved_errno = errno;
	close(dir_fd);

	if (sync_result == 0)
		return 0;

	/*
	 * fsync failed. Differentiate between unsupported operations
	 * and genuine storage/hardware failures.
	 */
	switch (saved_errno) {
	case EINVAL:
		//	case EOPNOTSUPP: // compiler treats as duplicate of ENOTSUP
	case ENOTSUP:
	case EROFS:
	case ENOSYS:
	case EBADF:
	case EISDIR:
		/*
		 * The OS or filesystem does not require or support
		 * directory syncing. Silently ignore and proceed.
		 */
		return 0;

	case EIO:
	case ENOSPC:
	case EDQUOT:
	default:
		/*
		 * Genuine storage failure. The directory metadata may
		 * not have reached the disk.
		 */
		errno = saved_errno;
		return -1;
	}
}

/*
 * Create a backup of path using backup_create(), then verify that the
 * backup was fully written, fsynced, and closed.
 *
 * On failure, remove the partial backup and return -1.
 */
int makeVerifiedBackup(const char *path, char *backup_path,
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

	if (copyFd(fd, bfd) == -1) {
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

	/*
	 * Best-effort directory sync to persist the backup's directory
	 * entry on filesystems that require it.
	 */
	if (syncParentDirectory(backup_path) == -1) {
		saved_errno = errno;
		unlink(backup_path);
		errno = saved_errno;
		return -1;
	}

	return 0;
}
