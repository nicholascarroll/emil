#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Runs inside AIX, from the unpacked payload (see stage.sh): install
# GCC, then build the editor and run the unit suites with the system
# make.  The exit status is the verdict; console.py reads it back.
#
# The bar is the same as on every other target, with nothing waived:
# the build and the suites are compiled with -Werror, every suite and
# both pty tests must pass, and a check that reports SKIP is a failure.

set -u

here=$(cd "$(dirname "$0")" && pwd)
PATH=/opt/freeware/bin:/usr/bin:/usr/sbin:/etc:/bin
export PATH

oslevel -s
df -m /opt /tmp

# The image's /opt (64 MB) and /tmp (128 MB) are sized for a base
# install; GCC alone needs more.  chfs grows them from free space in
# rootvg.
chfs -a size=+1024M /opt || exit 2
chfs -a size=+512M /tmp || exit 2

# GCC's post-install step regenerates its fixed headers against this
# system's.  The fixincl it runs was configured with sed at
# /opt/freeware/bin/sed, the Toolbox's GNU sed, which is not installed
# (see packages.txt).  Its sed scripts are upstream GCC's and written
# for any POSIX sed, so the system one stands in.
[ -e /opt/freeware/bin/sed ] || {
    mkdir -p /opt/freeware/bin && ln -s /usr/bin/sed /opt/freeware/bin/sed
} || exit 2

echo "=== installing gcc (see packages.txt for why --nodeps) ==="
rpm --import "$here"/RPM-GPG-KEY-IBM-AIX-Toolbox || exit 2
rpm -K "$here"/rpms/*.rpm || exit 2
rpm -Uv --nodeps --replacepkgs "$here"/rpms/*.rpm || exit 2
gcc --version | head -1 || exit 2

cd "$here/src" || exit 2
echo "=== build ==="
make CC=gcc CFLAGS=-Werror || exit 3
./emil --version || exit 3

echo "=== test ==="
# tee for a live log; the status comes back through a file because a
# pipeline's status is tee's.
{ make CC=gcc CFLAGS=-Werror test 2>&1; echo $? > test.rc; } | tee test.log
[ "$(cat test.rc)" -eq 0 ] || exit 4
if grep -n 'SKIP' test.log; then
    echo "guest.sh: a check was skipped; on AIX every check must run"
    exit 5
fi
echo "guest.sh: all checks ran and passed"
