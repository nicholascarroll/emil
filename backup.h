/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_BACKUP_H
#define EMIL_BACKUP_H

#include <stddef.h>

/* Backup creation for the save path (§3.21.2).
 *
 * This unit is deliberately free of editor state.  It takes a path and
 * a buffer for the chosen backup name, touches the filesystem, and
 * reports success or errno.  It knows nothing of struct buffer, the
 * global E, the minibuffer or the prompt system, which is what lets
 * tests/test_backup.c compile a copy of backup.c against fake syscalls
 * without linking the editor.  Keep it that way: anything needing to
 * ask the user a question belongs in the caller.
 *
 * Create a verified backup of `path`.
 *
 * The chosen name is written to `backup_path` (capacity
 * `backup_path_size`).  The first candidate is `path` with a trailing
 * tilde; if that exists, the final UTF-8 character of the path is
 * replaced by an ASCII letter, tried from 'z' down to 'a'.  Candidates
 * are created with O_EXCL, so an existing backup is never overwritten.
 *
 * The backup holds the target's current on-disk contents, not the
 * buffer's.  It is fully written, fsynced and closed before this
 * returns, so a caller may truncate the target afterwards.
 *
 * Returns 0 on success.  On failure returns -1 with errno set, having
 * removed any partial backup; `backup_path` is then meaningless.
 */
int makeVerifiedBackup(const char *path, char *backup_path,
		       size_t backup_path_size);

#endif /* EMIL_BACKUP_H */
