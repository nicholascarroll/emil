#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT
#
# Download the AIX 7.2 QEMU image, the GCC RPMs listed in packages.txt
# and the key IBM signs them with into <cache-dir>, verifying every
# file against its pinned sha256.  Files already present with the right
# hash are kept, so a restored CI cache costs nothing.
#
# Usage: [AIX_MIRROR=<url>] fetch.sh <cache-dir>
#
# AIX_MIRROR is an optional base URL holding copies of these files under
# their own names, side by side (a GitHub release does this naturally).
# It is tried first, and the original source after it.  The pins are
# the same either way, so a mirror cannot change what is installed.
#
# It exists because public.dhe.ibm.com is not always reachable from
# GitHub's runners: connections to it have timed out there while the
# image host, and IBM itself from elsewhere, answered at once.
#
# The image is tenox's prebuilt AIX 7.2 TL5 SP3 on QEMU's pseries
# machine, with QEMU binaries for the host bundled alongside.

set -eu

cache=${1:?usage: fetch.sh <cache-dir>}
here=$(cd "$(dirname "$0")" && pwd)

IMAGE_URL=https://files.42718.net/mirror/tenox/os/aix/Virtual/aix72pseries-qemu.tar.lz
IMAGE_SHA256=32f2d54cda8b3957f97b51c5736577899c95e98c8da8215ac7df408080bc9a26
TOOLBOX=https://public.dhe.ibm.com/aix/freeSoftware/aixtoolbox
# IBM AIX Toolbox OSS, 795153B3DEAD076AF00AFECD8B0EF14941EEA8E0.  The
# guest imports it and checks every RPM's signature before installing.
KEY=RPM-GPG-KEY-IBM-AIX-Toolbox
KEY_SHA256=3bde5f38499c5442cdd29e90b208e9ab31b28e4f7ad615abf9a7bde75e0c9c7d

mkdir -p "$cache/rpms"

# fetch <url> <dest> <sha256>: the mirror's copy if there is one,
# else <url>.  A host that does not answer within 20 s is given up on
# after a few tries, rather than after curl's default of two minutes
# a try.
fetch() {
    if [ -f "$2" ] && echo "$3  $2" | sha256sum -c --status; then
        echo "cached: $(basename "$2")"
        return 0
    fi
    rm -f "$2"
    for u in ${AIX_MIRROR:+"${AIX_MIRROR%/}/$(basename "$2")"} "$1"; do
        echo "fetching: $u"
        if curl -fsSL --connect-timeout 20 --retry 3 --retry-delay 5 \
                --retry-all-errors -o "$2.part" "$u"; then
            if echo "$3  $2.part" | sha256sum -c --status; then
                mv "$2.part" "$2"
                return 0
            fi
            echo "fetch.sh: sha256 mismatch for $u" >&2
        fi
        rm -f "$2.part"
    done
    echo "fetch.sh: could not get $(basename "$2")" >&2
    exit 1
}

fetch "$IMAGE_URL" "$cache/aix72pseries-qemu.tar.lz" "$IMAGE_SHA256"
fetch "$TOOLBOX/ezinstall/ppc/$KEY" "$cache/$KEY" "$KEY_SHA256"

grep -v -e '^#' -e '^[[:space:]]*$' "$here/packages.txt" |
while read -r sha path; do
    fetch "$TOOLBOX/RPMS/$path" "$cache/rpms/$(basename "$path")" "$sha"
done

# A version bump leaves the old RPM behind in a restored cache, and
# guest.sh installs whatever is in the directory.
for f in "$cache"/rpms/*.rpm; do
    grep -q "/$(basename "$f")\$" "$here/packages.txt" || {
        echo "removing stale: $(basename "$f")"
        rm -f "$f"
    }
done
