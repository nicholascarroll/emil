#!/bin/sh
# End-to-end smoke test for the WASIX build.
#
# Usage: ./tests/wasix/smoke.sh [path-to-emil.wasm]
#
# The unit suites run under wasmer too (see `make wasix-test`), but they
# link stubs.o in place of main.o and terminal.o, so none of them starts
# the real binary.  This does, and drives it with keystrokes on stdin --
# emil does not require a pty, so no terminal emulation is needed and
# the result is deterministic.
#
# The file round-trip is the point.  The failure this exists to catch is
# a toolchain mismatch between wasi-sdk and wasix-libc: it links without
# a diagnostic and then traps inside __wasilibc_find_relpath_alloc on
# the first open(), because the prebuilt libc keeps path-resolution
# state in thread-local storage that an older wasm-ld lays out wrongly.
# A build-only CI job would go green on a binary that cannot open a
# file.  Opening, editing and saving one is the cheapest thing that
# distinguishes the two.
set -eu

WASM=${1:-./emil.wasm}
WASMER=${WASMER:-wasmer}

if [ ! -f "$WASM" ]; then
    echo "✗ $WASM not found (run: make wasix)" >&2
    exit 1
fi
if ! command -v "$WASMER" >/dev/null 2>&1; then
    echo "✗ wasmer not found on PATH (run: ./tests/wasix/setup.sh)" >&2
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
cp "$WASM" "$WORK/emil.wasm"

fail=0
ok()   { printf '  %-44s PASS\n' "$1"; }
bad()  { printf '  %-44s FAIL\n' "$1"; [ $# -lt 2 ] || printf '      %s\n' "$2"; fail=1; }

echo "wasix_smoke: driving $WASM under $WASMER"

# --- 1. the binary runs at all --------------------------------------
#
# stderr is folded in so a real failure is visible, then the emil line
# is picked out of it.
#
# --volume, not --dir: wasmer 7 warns that --dir is deprecated and 8
# removes it.  An earlier version of this file kept --dir and claimed
# --volume "does not make the mapped directory reachable by the paths
# the editor is given, so file writes silently do nothing".  That is
# true only of the naive form, --volume .:/somewhere, which maps the
# directory without setting the guest's working directory, so a
# relative path resolves somewhere else and the write is discarded.
# --volume "$WORK:$WORK" --cwd "$WORK" maps the directory at its own
# path and starts the guest there; a relative filename then reaches
# the host file.  Verified by round-tripping a save through both forms
# and comparing the host file.
raw=$("$WASMER" run ./emil.wasm --volume "$WORK:$WORK" --cwd "$WORK" -- --version 2>&1 </dev/null || true)
version=$(printf '%s\n' "$raw" | grep '^emil ' || true)
case "$version" in
    emil\ *) ok "binary runs and reports a version" ;;
    *)       bad "binary runs and reports a version" "got: $raw" ;;
esac

# Version agreement with the Makefile, same check the native suite makes.
mk_version=$(awk -F'=' '/^VERSION/ {gsub(/[ \t]/, "", $2); print $2; exit}' Makefile)
bin_version=$(printf '%s' "$version" | awk '/emil/ {print $2; exit}')
if [ "$bin_version" = "$mk_version" ]; then
    ok "version matches Makefile ($mk_version)"
else
    bad "version matches Makefile" "binary=$bin_version makefile=$mk_version"
fi

# --- 2. open, edit, save --------------------------------------------
#
# Keystrokes: type "ZZZ", then C-x C-s (save), then C-x C-c (quit).
# 030 = C-x, 023 = C-s, 003 = C-c.
printf 'alpha\nbeta\n' > "$WORK/round.txt"
( cd "$WORK" && printf 'ZZZ\030\023\030\003' \
    | "$WASMER" run ./emil.wasm --volume "$WORK:$WORK" --cwd "$WORK" -- round.txt >/dev/null 2>&1 ) || true

expected=$(printf 'ZZZalpha\nbeta\n')
actual=$(cat "$WORK/round.txt")
roundtrip_ok=0
if [ "$actual" = "$expected" ]; then
    roundtrip_ok=1
    ok "open, edit and save round-trip to disk"
else
    bad "open, edit and save round-trip to disk" \
        "expected [$expected] got [$actual]"
fi

# --- 3. the save cleaned up its backup ------------------------------
#
# emil's save is not atomic and uses no temporary file: it writes a
# verified backup, overwrites the target in place, then unlinks the
# backup (EMIL-DESIGN.md 3.21.2).  A surviving round.txt~ means the
# unlink half did not happen.
#
# This check previously looked for round.txt.tmp*, a mkstemp-and-rename
# pattern emil has never used, so it could not fail.  The wasm binary
# imports no path_rename at all, which is the cheapest way to see it.
# It does import path_unlink_file, so the path asserted here is live.
#
# Absence of a backup is only evidence of cleanup if a save actually
# ran, so this check is gated on check 2.  Reporting PASS here after
# check 2 failed would be the same vacuous green the old check gave.
leftovers=$(find "$WORK" -name 'round.txt~' 2>/dev/null | wc -l | tr -d ' ')
if [ "$roundtrip_ok" != "1" ]; then
    bad "save removed its backup file" \
        "not established: the save in check 2 did not happen"
elif [ "$leftovers" = "0" ]; then
    ok "save removed its backup file"
else
    bad "save removed its backup file" "$leftovers left behind"
fi

# --- 4. UTF-8 survives the round trip -------------------------------
#
# Multi-byte input exercises the decoder against the runtime's stdin
# rather than against the unit tests' synthetic key scripts.
printf 'seed\n' > "$WORK/utf8.txt"
( cd "$WORK" && printf '\346\227\245\346\234\254\030\023\030\003' \
    | "$WASMER" run ./emil.wasm --volume "$WORK:$WORK" --cwd "$WORK" -- utf8.txt >/dev/null 2>&1 ) || true
if head -n 1 "$WORK/utf8.txt" | grep -q '日本'; then
    ok "UTF-8 input round-trips to disk"
else
    bad "UTF-8 input round-trips to disk" "got: $(head -n 1 "$WORK/utf8.txt")"
fi

echo ""
if [ "$fail" -ne 0 ]; then
    echo "wasix_smoke: FAILED"
    exit 1
fi
echo "wasix_smoke: all checks passed"
