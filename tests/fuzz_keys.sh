#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
# fuzz_keys.sh: build tests/fuzz_keys.c against the editor's sources under
# ASan+UBSan, in a scratch directory, and run it from a sandbox directory
# holding a few files for the file, completion and ctags commands to find.
#
#   tests/fuzz_keys.sh [FIRST_SEED [COUNT]]
#
# A pre-1.0 audit tool; not part of `make test`.  Leaves the repository's
# own objects alone.  Run it as an unprivileged user: the key stream can
# type any file name into C-x C-w.  HOME is pointed at the sandbox so a
# typed "~/..." lands there rather than in your real home directory.
set -eu
CC=${CC:-cc}
FIRST=${1:-1}
COUNT=${2:-1000}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORK=${FUZZ_WORK:-${TMPDIR:-/tmp}/emil-fuzz-keys}
mkdir -p "$WORK/obj" "$WORK/sandbox/sub"

CFLAGS="-std=c99 -D_DEFAULT_SOURCE -D_BSD_SOURCE -g -O1 \
    -fsanitize=address,undefined -fno-sanitize-recover=all \
    -fno-omit-frame-pointer -DEMIL_DEBUG_ROW_CACHE -DEMIL_VERSION=\"fuzz\""

OBJS=""
for src in "$ROOT"/*.c; do
    base=$(basename "$src" .c)
    case $base in main|terminal) continue ;; esac
    obj="$WORK/obj/$base.o"
    if [ ! -f "$obj" ] || [ "$src" -nt "$obj" ]; then
        $CC $CFLAGS -I"$ROOT" -c "$src" -o "$obj"
    fi
    OBJS="$OBJS $obj"
done
$CC $CFLAGS -I"$ROOT" -o "$WORK/fuzz_keys" "$ROOT/tests/fuzz_keys.c" $OBJS

cd "$WORK/sandbox"
HOME=$(pwd)
export HOME
printf 'int foo(int x);\n' > a.h
printf '#include "a.h"\nint foo(int x) { return x + 1; }\nint main(void) { return foo(1); }\n' > a.c
printf 'Some notes.\nA second line with \xc3\xa9t\xc3\xa9.\n' > notes.txt
printf 'one\n' > sub/one.txt
printf 'two\n' > sub/two.txt
printf 'foo\ta.c\t/^int foo(int x) {$/;"\tf\nfoo\ta.h\t1;"\tp\nmain\ta.c\t3;"\tf\nalpha\tnotes.txt\t1\n' > tags

ASAN_OPTIONS=${ASAN_OPTIONS:-detect_leaks=0:abort_on_error=0:exitcode=99}
UBSAN_OPTIONS=${UBSAN_OPTIONS:-print_stacktrace=1}
export ASAN_OPTIONS UBSAN_OPTIONS
if [ "${FUZZ_BUILD_ONLY:-}" ]; then
    echo "$WORK/fuzz_keys"
    exit 0
fi
exec "$WORK/fuzz_keys" run "$FIRST" "$COUNT"
