#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Inject the emil payload into Asterinas's initramfs image.
#
# Usage: inject.sh <asterinas-dir> <payload-dir>
#
# Why this exists rather than a Nix expression.  Asterinas builds its
# initramfs with Nix, and the supported way to add a program is to
# write a .nix under test/initramfs/nix/regression and have it packaged
# in.  That is the right thing for a test that lives in the Asterinas
# tree; it is the wrong thing for a test that lives here, because it
# would mean carrying a fork of their expressions and re-resolving them
# on every upstream bump.  Unpacking and repacking the cpio touches
# nothing upstream and breaks loudly if the layout changes.
#
# Two details make this fiddly and are worth stating plainly:
#
#   1. `make initramfs` produces build/initramfs.cpio.gz as a *symlink*
#      into the read-only Nix store.  It cannot be edited in place; it
#      has to be replaced with a real file.
#
#   2. That target is declared .PHONY in test/initramfs/Makefile, so a
#      later `make run_kernel` re-runs nix-build and restores the
#      symlink -- silently discarding the injection.  The CI job
#      therefore runs the kernel with `make --old-file=initramfs`,
#      which is the load-bearing flag in this whole arrangement.  If a
#      future Asterinas reorganises those targets, the symptom is a
#      clean boot with no emil in it.

set -eu

aster=${1:?usage: inject.sh <asterinas-dir> <payload-dir>}
payload=${2:?usage: inject.sh <asterinas-dir> <payload-dir>}

image="$aster/test/initramfs/build/initramfs.cpio.gz"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

[ -e "$image" ] || {
	echo "inject.sh: no initramfs at $image (run 'make initramfs' first)" >&2
	exit 1
}

echo "--- Unpacking initramfs ---"
mkdir -p "$work/root"
# `cpio -i` reads from stdin; -d makes directories, -m preserves
# mtimes, --no-absolute-filenames keeps a malformed archive from
# writing outside the work directory.
gzip -cd "$image" | (cd "$work/root" && cpio -idm --no-absolute-filenames 2>/dev/null)

# Sanity: the archive should contain the init script the kernel is
# pointed at.  If it does not, the layout has changed and repacking
# would produce something that boots to nothing.
[ -f "$work/root/test/boot_hello.sh" ] || {
	echo "inject.sh: /test/boot_hello.sh not found in the initramfs." >&2
	echo "           Asterinas's initramfs layout has changed; the" >&2
	echo "           AUTO_TEST=boot entry point in ci.yml needs revisiting." >&2
	exit 1
}

echo "--- Injecting payload ---"
rm -rf "$work/root/emil"
mkdir -p "$work/root/emil"
cp -a "$payload"/. "$work/root/emil/"

# The kernel is launched with --init-args=/test/boot_hello.sh (see
# AUTO_TEST=boot in Asterinas's Makefile).  Rather than teach the
# Makefile a new AUTO_TEST value -- which would be a fork of their
# tree -- the entry point is replaced with one that hands over to the
# payload.  exec, so the runner is pid 1 and its exit is the system's.
cat > "$work/root/test/boot_hello.sh" <<'EOF'
#!/bin/sh
# Replaced by emil's CI; see asterinas/inject.sh.
exec /emil/run_suites.sh
EOF
chmod +x "$work/root/test/boot_hello.sh"

echo "--- Repacking initramfs ---"
# Replace the store symlink with a real file.  rm first: redirecting
# onto a symlink would write through into the Nix store, which is
# read-only and would fail here rather than at boot -- but only by
# luck, so do not rely on it.
rm -f "$image"
(cd "$work/root" && find . | cpio -o -H newc 2>/dev/null) | gzip -9 > "$image"

echo "  $(du -h "$image" | cut -f1) at $image"
