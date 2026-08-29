#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Assemble tests/genode/emil/src/ from the sources at the repository
# root.
#
# Goa requires each project's sources to live in <project>/src, and binds
# that directory into a bubblewrap sandbox read-only.  The editor's
# sources live at the repository root, so they have to be brought in.
# They are copied rather than symlinked: a symlink out of the project
# resolves to a path the sandbox has not bound, so the compiler reports a
# missing file rather than a missing bind.
#
# Both the editor sources and tests/ are staged.  The Genode build has
# its own Makefile (Makefile.in): the top-level one carries native,
# wasix and target-stamp logic that means nothing under a cross
# toolchain goa drives itself.

set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)
src="$here/emil/src"

# The two levels above are load-bearing since this directory moved under
# tests/.  A wrong root would stage an empty tree and the failure would
# surface much later, as a link error inside the sandbox.
[ -f "$root/Makefile" ] && [ -f "$root/emil.h" ] || {
	echo "stage.sh: $root is not the repository root" >&2
	exit 1
}

rm -rf "$src"
mkdir -p "$src/tests"

# The wildcards in Makefile.in pick up whatever lands here, so a new
# translation unit at the root, or a new tests/test_*.c, needs no change
# to this script or to it.
n=0
for f in "$root"/*.c "$root"/*.h; do
	[ -e "$f" ] || continue
	cp -- "$f" "$src/"
	n=$((n + 1))
done

if [ "$n" -eq 0 ]; then
	echo "stage.sh: no sources found in $root" >&2
	exit 1
fi

t=0
for f in "$root"/tests/*.c "$root"/tests/*.h; do
	[ -e "$f" ] || continue
	cp -- "$f" "$src/tests/"
	t=$((t + 1))
done

if [ "$t" -eq 0 ]; then
	echo "stage.sh: no test sources found in $root/tests" >&2
	exit 1
fi

# The version is defined in exactly one place -- VERSION in the
# top-level Makefile -- and reaches the program as -DEMIL_VERSION.
# Without it emil.h's fallback takes over and the build reports itself
# as "unknown", which is a silent defect: it links, runs, and lies.
version=$(sed -n 's/^VERSION[[:space:]]*=[[:space:]]*\(.*\)/\1/p' "$root/Makefile" | head -1)
if [ -z "$version" ]; then
	echo "stage.sh: could not read VERSION from $root/Makefile" >&2
	exit 1
fi
printf 'EMIL_VERSION = %s\n' "$version" > "$src/version.mk"

# Generated fixture: backup.c against fake syscalls.  test_backup.c
# #includes the result rather than linking it, so it has to exist before
# the cross build starts -- which is why this is not left to
# run_tests.sh, a script that knows nothing about the goa toolchain.
# The generation itself is shared; see tests/make_backup_fixture.sh.
sh "$root/tests/make_backup_fixture.sh" \
	"$root/backup.c" "$src/tests/backup_faked.c" >/dev/null

cp -- "$here/emil/Makefile.in" "$src/Makefile"

echo "stage.sh: staged $n sources and $t test files into $src (version $version)"
