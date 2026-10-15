#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Generate tests/backup_faked.c: a copy of backup.c whose syscalls are
# rewritten to the fk_* fakes that tests/test_backup.c defines.
#
# Usage: make_backup_fixture.sh <backup.c> <output.c>
#

set -eu

src=${1:?usage: make_backup_fixture.sh <backup.c> <output.c>}
out=${2:?usage: make_backup_fixture.sh <backup.c> <output.c>}

BACKUP_SYSCALLS="open close read write fsync unlink fcntl"
BACKUP_EXPECTED_SITES=22

sed_script=""
for fn in $BACKUP_SYSCALLS; do
	sed_script="$sed_script -e s/\\([^a-zA-Z0-9_]\\)$fn(/\\1fk_$fn(/g"
done
sed_script="$sed_script -e s/makeVerifiedBackup/fake_makeVerifiedBackup/g"

# shellcheck disable=SC2086
sed $sed_script "$src" > "$out"

sites=$(grep -c 'fk_' "$out")
if [ "$sites" -ne "$BACKUP_EXPECTED_SITES" ]; then
	echo "make_backup_fixture.sh: expected $BACKUP_EXPECTED_SITES call" >&2
	echo "  sites in $src, rewrote $sites.  backup.c changed shape --" >&2
	echo "  update BACKUP_EXPECTED_SITES here after checking that" >&2
	echo "  tests/test_backup.c still fakes every syscall it now uses." >&2
	exit 1
fi
if grep -q '[^a-zA-Z0-9_]makeVerifiedBackup(' "$out"; then
	echo "make_backup_fixture.sh: rewrite left an un-renamed" >&2
	echo "  makeVerifiedBackup in $out" >&2
	exit 1
fi

echo "$sites"
