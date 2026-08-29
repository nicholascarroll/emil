#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Generate tests/backup_faked.c: a copy of backup.c whose syscalls are
# rewritten to the fk_* fakes that tests/test_backup.c defines (#125).
#
# Usage: make_backup_fixture.sh <backup.c> <output.c>
#
# backup.c carries no test hooks and no test-only macros; the editor
# source is what ships.  Fault injection is done by rewriting a copy, so
# the suite drives the real control flow over a filesystem that fails on
# demand.
#
# makeVerifiedBackup is renamed too.  test_backup.c #includes this copy
# rather than linking it -- that is what makes the file-local helpers
# reachable -- and the real backup.o is also linked in, so without the
# rename the two definitions would collide.
#
# The substitution is deliberately blunt, so it is checked rather than
# trusted: the expected number of call sites is asserted, and drift in
# either direction fails here instead of silently testing the real
# filesystem.  BRE with an explicit leading-character class, not \b,
# which is a GNU extension that BSD and Solaris sed lack.
#
# This lives in its own script because two builds need it and neither
# can call the other: tests/run_tests.sh generates it for the native and
# cross-compiled runs, and tests/genode/stage.sh generates it before the
# goa build starts, without running run_tests.sh under a cross toolchain
# it knows nothing about.  It was duplicated in both, with the call-site
# count written out twice.

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
