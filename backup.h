/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_BACKUP_H
#define EMIL_BACKUP_H

#include <stddef.h>

/* Create a verified backup of `path`.
 *
 * The chosen name is written to `backup_path` (capacity
 * `backup_path_size`).  The first candidate is `path` with a trailing
 * tilde; if that exists, the final UTF-8 character of the path is
 * replaced by an ASCII letter, tried from 'z' down to 'a'.  Candidates
 * are created with O_EXCL, so an existing backup is never overwritten.
 *
 * Returns 0 on success.  On failure returns -1 with errno set, having
 * removed any partial backup; `backup_path` is then meaningless.
 */
int makeVerifiedBackup(const char *path, char *backup_path,
		       size_t backup_path_size);

#endif /* EMIL_BACKUP_H */
