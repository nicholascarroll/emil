#!/bin/sh
# Fetch the Redox OS cross toolchain into $PREFIX (default $HOME/opt).
#
# Usage:  ./tests/redox_setup.sh [prefix] [target]
# Then:   make redox REDOX_TOOLCHAIN=<prefix>/redox-toolchain
#
# Two archives are needed and they are not interchangeable: gcc-install
# carries the cross compiler and binutils, relibc-install carries the
# sysroot (headers plus libc) that compiler links against.  Both unpack
# into the same prefix.
#
# Nothing here is version-pinned, because upstream publishes nothing to
# pin to.  The archives live at a fixed URL, carry no version in their
# name, and are rebuilt nightly; the only identifier available is the
# SHA256SUM file sitting next to them, which is what this script
# records.  That is worth knowing before reading the retry loop below:
# it is not defensive programming for its own sake, it is the direct
# consequence of an unversioned nightly artifact served from a cache.
set -eu

PREFIX=${1:-$HOME/opt}
TARGET=${2:-x86_64-unknown-redox}
BASE=https://static.redox-os.org/toolchain/$TARGET
DEST=$PREFIX/redox-toolchain

# The two archives, and the compiler that proves the first one landed.
ARCHIVES="gcc-install.tar.gz relibc-install.tar.gz"
CC_BIN=$DEST/bin/$TARGET-gcc

# sha256sum on Linux, shasum -a 256 on macOS.
if command -v sha256sum >/dev/null 2>&1; then
    sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    echo "redox_setup.sh: no sha256sum or shasum on PATH" >&2
    exit 1
fi

if [ -x "$CC_BIN" ] && [ -d "$DEST/$TARGET/include" ]; then
    echo "redox_setup.sh: toolchain already present at $DEST"
    "$CC_BIN" --version | head -1
    exit 0
fi

mkdir -p "$PREFIX"
cd "$PREFIX"

# --- fetch and verify ------------------------------------------------
#
# The archives and the SHA256SUM that describes them are published
# separately, seconds apart, unversioned, from behind a cache.  A run
# that starts either side of a nightly rebuild can therefore hold a
# stale checksum against a fresh archive (or the reverse) and see a
# mismatch on a download that is perfectly intact.  Observed directly:
# relibc-install.tar.gz and SHA256SUM two seconds apart in Last-Modified,
# a complete archive whose size matched Content-Length exactly, and a
# hash that did not match until SHA256SUM was fetched again.
#
# So a mismatch is not treated as corruption on the first pass.  The
# integrity of the download is established independently, by gzip -t,
# which catches the truncation a checksum would otherwise be carrying;
# the checksum is then used for what it can actually tell us here,
# which is whether the two archives came from the same nightly build.
# Only a mismatch that survives a re-fetch of both is real.
verified=no
attempt=1
while [ "$attempt" -le 3 ]; do
    echo ""
    echo "redox_setup.sh: fetch attempt $attempt of 3"

    for a in $ARCHIVES; do
        if [ ! -f "$a" ]; then
            echo "  downloading $a"
            curl -fL --no-progress-meter \
                 --retry 5 --retry-delay 5 --retry-all-errors \
                 -o "$a.part" "$BASE/$a"
            mv "$a.part" "$a"
            echo "    $(wc -c < "$a") bytes"
        else
            echo "  $a already on disk"
        fi
    done

    # Integrity first, and on its own terms.  A truncated archive is a
    # real failure at any attempt count, so it is never retried against
    # a fresh checksum -- it is re-downloaded.
    truncated=""
    for a in $ARCHIVES; do
        if ! gzip -t "$a" 2>/dev/null; then
            echo "  ✗ $a is not a complete gzip stream; discarding"
            rm -f "$a"
            truncated="yes"
        fi
    done
    if [ -n "$truncated" ]; then
        attempt=$((attempt + 1))
        continue
    fi
    echo "  ✓ both archives are complete gzip streams"

    # Cache-busted, so a retry cannot be served the same stale copy
    # that caused the retry.
    curl -fsSL --retry 5 --retry-all-errors \
         -o SHA256SUM "$BASE/SHA256SUM?cb=$(date +%s)-$attempt"

    mismatch=""
    for a in $ARCHIVES; do
        want=$(sed -n "s/^\([0-9a-f]\{64\}\)[[:space:]]*[*]\{0,1\}$a\$/\1/p" \
               SHA256SUM | head -1)
        got=$(sha256_of "$a")
        if [ -z "$want" ]; then
            echo "  ! SHA256SUM has no line for $a"
            mismatch="yes"
        elif [ "$want" = "$got" ]; then
            echo "  ✓ $a matches ($got)"
        else
            echo "  ! $a differs from the published checksum"
            echo "      published: $want"
            echo "      on disk:   $got"
            mismatch="yes"
        fi
    done

    if [ -z "$mismatch" ]; then
        verified=yes
        break
    fi

    # Mismatch on an intact archive means the pair straddles a nightly
    # rebuild.  Drop both and take the next build whole, rather than
    # mixing a compiler from one night with a sysroot from another.
    echo "  ! archives and checksum disagree; assuming a publish race"
    echo "    and re-fetching both so they come from one build"
    rm -f $ARCHIVES
    attempt=$((attempt + 1))
done

if [ "$verified" != yes ]; then
    echo "" >&2
    echo "redox_setup.sh: could not obtain a self-consistent toolchain" >&2
    echo "  after 3 attempts.  Both archives were complete gzip streams" >&2
    echo "  but did not match the published SHA256SUM, which points at" >&2
    echo "  upstream rather than at the download.  Check $BASE/" >&2
    exit 1
fi

# --- unpack ----------------------------------------------------------
#
# Both archives share a layout (bin/, $TARGET/, lib/, libexec/) and are
# meant to be unpacked over one another into a single prefix.
echo ""
echo "redox_setup.sh: unpacking into $DEST"
rm -rf "$DEST"
mkdir -p "$DEST"
for a in $ARCHIVES; do
    echo "  $a"
    tar -xzf "$a" -C "$DEST"
done

# Record which nightly this is.  The toolchain carries no version of its
# own, so without this a CI log cannot say which build it used, and two
# runs that behaved differently cannot be told apart afterwards.
cp SHA256SUM "$DEST/SHA256SUM.provenance"

if [ ! -x "$CC_BIN" ]; then
    echo "redox_setup.sh: no compiler at $CC_BIN after unpacking" >&2
    echo "  the archive layout may have changed upstream" >&2
    exit 1
fi

echo ""
echo "redox_setup.sh: toolchain ready in $DEST"
"$CC_BIN" --version | head -1
echo "sysroot: $DEST/$TARGET"
