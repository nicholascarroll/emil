#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Pack the source tree, the GCC RPMs, IBM's signing key and guest.sh
# into a CD-ROM image for the AIX VM.
#
# Usage: stage.sh <cache-dir> <out.iso>
#
# <cache-dir> is the one fetch.sh filled.
#
# The VM has no configured network, and the console is too slow to
# carry a source tree, so the payload goes in on the CD-ROM drive the
# image already defines.  Everything is in one tar inside the ISO so
# that nothing depends on how AIX's cdrfs maps long or mixed-case file
# names; ustar keeps the archive readable by AIX's own tar.

set -eu

cache=${1:?usage: stage.sh <cache-dir> <out.iso>}
iso=${2:?usage: stage.sh <cache-dir> <out.iso>}
here=$(cd "$(dirname "$0")" && pwd)
top=$(cd "$here/../.." && pwd)

work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

mkdir -p "$work/emil-aix/rpms" "$work/emil-aix/src"
cp "$cache"/rpms/*.rpm "$work/emil-aix/rpms/"
cp "$cache"/RPM-GPG-KEY-IBM-AIX-Toolbox "$work/emil-aix/"
cp "$here/guest.sh" "$work/emil-aix/"
# The working tree rather than git archive, so that a local run tests
# uncommitted changes too.  Build products are left behind, and so is
# the CI job's download cache, which lives in the workspace.
(cd "$top" && tar --exclude=.git --exclude='*.o' --exclude=./emil \
    --exclude=./tests/tmp --exclude=.build-tag --exclude=./.aix-cache \
    -cf - .) |
    (cd "$work/emil-aix/src" && tar xf -)

mkdir "$work/iso"
(cd "$work" && tar --format=ustar -cf iso/payload.tar emil-aix)
genisoimage -quiet -R -V EMILAIX -o "$iso" "$work/iso"
ls -l "$iso"
