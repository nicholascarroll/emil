#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
# build.sh: build one commit of emil as a refcheck target.
#
#   refcheck/build.sh REPO COMMIT OUTDIR
#
# Checks COMMIT out into OUTDIR/src (a detached git worktree), compiles
# its objects under ASan+UBSan into OUTDIR/obj, and links this checkout's
# refcheck/difffuzz.c against them as OUTDIR/difffuzz.  A second,
# coverage-instrumented copy goes to OUTDIR/cov/difffuzz; it is used
# only to see which changed lines a witness executes.  The driver
# source always comes from the refcheck being run, never from COMMIT, so
# the old and new builds are driven by identical code.
set -eu
REPO=$1
COMMIT=$2
OUT=$3
HERE=$(cd "$(dirname "$0")" && pwd)
CC=${CC:-cc}
JOBS=${JOBS:-$(nproc 2>/dev/null || echo 2)}

mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)
if [ ! -d "$OUT/src" ]; then
    git -C "$REPO" worktree add --detach --force "$OUT/src" "$COMMIT" >/dev/null 2>&1 ||
        git -C "$REPO" worktree add --detach "$OUT/src" "$COMMIT"
fi
git -C "$OUT/src" rev-parse HEAD > "$OUT/commit"
mkdir -p "$OUT/obj"

# A generated makefile does the parallel compile, so flags need no
# shell re-quoting and only changed sources are rebuilt.
write_mk() { # write_mk OBJDIR BIN FLAGS
    # Rewritten only when it changes; objects depend on it, so changed
    # flags rebuild everything and unchanged flags rebuild nothing.
    cat > "$1/build.mk.new" <<MK
CC = $CC
CFLAGS = -std=c99 -D_DEFAULT_SOURCE -D_BSD_SOURCE -U_FORTIFY_SOURCE -g $3 \\
	'-DEMIL_VERSION="refcheck"' -I$OUT/src
SRCS = \$(filter-out $OUT/src/main.c $OUT/src/terminal.c,\$(wildcard $OUT/src/*.c))
OBJS = \$(patsubst $OUT/src/%.c,$1/%.o,\$(SRCS))
$2: \$(OBJS) $HERE/difffuzz.c
	\$(CC) \$(CFLAGS) -o \$@ $HERE/difffuzz.c \$(OBJS) -lvterm
$1/%.o: $OUT/src/%.c $1/build.mk
	\$(CC) \$(CFLAGS) -c \$< -o \$@
MK
    if cmp -s "$1/build.mk.new" "$1/build.mk"; then
        rm -f "$1/build.mk.new"
    else
        mv "$1/build.mk.new" "$1/build.mk"
    fi
    make -s -j"$JOBS" -f "$1/build.mk" > "$1/compile.log" 2>&1 || {
        cat "$1/compile.log" >&2
        echo "build.sh: build failed for $COMMIT" >&2
        exit 1
    }
}
mkdir -p "$OUT/cov/obj"
write_mk "$OUT/obj" "$OUT/difffuzz" "-O1 -fsanitize=address,undefined \
	-fno-sanitize-recover=all -fno-omit-frame-pointer -DEMIL_DEBUG_ROW_CACHE -DEMIL_DEBUG_ROW_BYTES"
write_mk "$OUT/cov/obj" "$OUT/cov/difffuzz" "-O0 --coverage"
echo "$OUT/difffuzz"
