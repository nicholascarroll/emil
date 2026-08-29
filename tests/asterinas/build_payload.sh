#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Build the editor and its unit suites for Asterinas and stage them
# into a payload directory, ready to be injected into the initramfs.
#
# Usage: build_payload.sh <payload-dir>
#
# This is not a cross build.  Asterinas implements the Linux syscall
# ABI on x86-64, and the development container is x86-64 Linux, so the
# host compiler is the target compiler and what is produced here are
# ordinary Linux ELF binaries.  That is the point of the target: no
# toolchain to fetch, no libc to port, no EMIL_DISABLE_SHELL.  Redox
# asks whether the editor builds against a libc written in Rust; this
# asks whether the binary it already builds still works when the
# kernel underneath it is written in Rust.
#
# The suites are built by run_tests.sh rather than by a loop here, so
# that the source invariants, the suite-list count check and the
# generated backup.c fixture all happen exactly once, in the place
# that already owns them.  BUILD_ONLY compiles and links without
# running; KEEP_BUILT leaves the results on disk to be staged.
#
# Everything is linked -static.  The initramfs is Nix-built, so its
# only ELF interpreters live at /nix/store paths this container does
# not share; a dynamically linked binary would link cleanly here and
# fail at exec inside the VM.
#
# Known cost of -static, deliberately not worked around: glibc's NSS is
# dlopen-based, so a static binary that calls getpwnam/getpwuid
# resolves nothing at runtime.  The editor reaches those only for ~
# expansion, and test_tilde is the suite that would notice.  If
# test_tilde fails on the first green run, that is glibc-static and not
# Asterinas -- the fix is to build against musl, not to bend the test.

set -eu

payload=${1:?usage: build_payload.sh <payload-dir>}

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)

# Two levels up since this directory moved under tests/.  A wrong root
# would build nothing and stage an empty payload, and the failure would
# surface twenty minutes later as a VM that boots to no suites.
[ -f "$root/Makefile" ] && [ -f "$root/emil.h" ] || {
	echo "build_payload.sh: $root is not the repository root" >&2
	exit 1
}
cd "$root"

CC=${CC:-gcc}
STATIC="-static"

# Mirrors DEFAULT_CFLAGS in the Makefile; run_tests.sh appends -I. and
# the test-specific flags itself.
CFLAGS_TARGET="-std=c99 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE -D_BSD_SOURCE -O2"

echo "--- Building emil (static) ---"
make CC="$CC" LDFLAGS="$STATIC" BUILD_TAG=asterinas

# Confirm it really is static before anything downstream depends on it.
# `file` is not in every image; ldd is, and on a static binary it
# fails -- which is the success case here.
if ldd ./emil >/dev/null 2>&1; then
	echo "build_payload.sh: emil is dynamically linked; it will not exec" >&2
	echo "                  inside the Nix-built initramfs." >&2
	exit 1
fi

echo "--- Building the unit suites (static, not run) ---"
BUILD_ONLY=1 KEEP_BUILT=1 \
	CC="$CC" CFLAGS="$CFLAGS_TARGET" LDFLAGS="$STATIC" \
	./tests/run_tests.sh

# The pty scenarios.  run_tests.sh does not run these here, because on a
# cross target the host cannot exec the editor.  The binary is native to
# the guest, so the harness is built and shipped and run there.
# pty_input_test only: Asterinas has a pty subsystem with termios and a
# line discipline, which is what these need; pty_signals_test needs the
# master to reflect the slave and real job control, and is Linux/macOS.
# shellcheck disable=SC2086
$CC $CFLAGS_TARGET -I. -Itests -o tests/pty_input_test \
	tests/pty_input_test.c $STATIC

echo "--- Staging $payload ---"
mkdir -p "$payload/tests"
cp ./emil "$payload/emil"
cp "$here/run_suites.sh" "$payload/run_suites.sh"
chmod +x "$payload/run_suites.sh"

staged=0
for bin in tests/test_* tests/pty_input_test; do
	# tests/test_*.c matches the glob too; only take what was linked.
	[ -f "$bin" ] && [ -x "$bin" ] || continue
	case "$bin" in *.c | *.h) continue ;; esac
	cp "$bin" "$payload/tests/"
	staged=$((staged + 1))
done

# The floor used to be 1, which any two files satisfied.  Nothing is
# compiled out for this target, so every suite that exists should have
# been linked and staged; 30 leaves room for a few being trimmed without
# letting a mostly-empty payload through.  The guest runner asserts the
# same floor again on what it actually finds.
[ "$staged" -ge 30 ] || {
	echo "build_payload.sh: staged $staged binaries; expected the whole" >&2
	echo "                  suite set.  Did KEEP_BUILT reach run_tests.sh?" >&2
	exit 1
}

echo "  staged $staged binaries"
ls -la "$payload"
