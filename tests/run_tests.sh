#!/bin/sh
set -u

CC=${CC:-cc}
CFLAGS=${CFLAGS:-""}
LDFLAGS=${LDFLAGS:-""}

# Cross-target support.  All three default to the native behaviour.
#
#   PROGNAME     name of the built editor binary (the wasix target
#                builds emil.wasm rather than emil).
#   RUNNER       command prefix used to execute a built binary.  Empty
#                natively; for a Wasm target this is the host runtime,
#                e.g. RUNNER="wasmer run --dir .". 
#   RUNNER_SEP   separator inserted between the binary and its own
#                arguments. 
#   NM           nm to use for the leaf-module invariant.  The host nm
#                cannot read wasm objects, so a cross build must supply
#                its own, e.g. NM=$WASI_SDK/bin/llvm-nm.
#   BUILD_ONLY   compile and link everything but do not run it.
#   KEEP_BUILT   with BUILD_ONLY, leave the linked suites on disk
#                instead of deleting each one after it links, so they
#                can be staged.  No effect without BUILD_ONLY.
PROGNAME=${PROGNAME:-emil}
RUNNER=${RUNNER:-}
RUNNER_SEP=${RUNNER_SEP:-}
NM=${NM:-nm}
BUILD_ONLY=${BUILD_ONLY:-}
KEEP_BUILT=${KEEP_BUILT:-}

# Runners that communicate through a virtual machine's console rather than
# returning a process status.
#
#   RUNNER_CONSOLE  the runner cannot be trusted to report the child's
#                   exit status, and its console ends lines with CRLF.
#                   Verdicts are then taken from what the suite printed
#                   -- the TEST_END() summary and its OK/FAIL line --
#                   rather than from $?.
#   RUNNER_MARKER   a line the runner prints immediately before the
#                   child's own output.  Everything up to and including
#                   the last occurrence is dropped, so boot messages
#                   cannot be mistaken for test output.  Ignored if the
#                   marker never appears, which keeps the whole log when
#                   the runner failed before starting the child.
#
RUNNER_CONSOLE=${RUNNER_CONSOLE:-}
RUNNER_MARKER=${RUNNER_MARKER:-}

# Strip a VM console's CRLF, and drop everything the runner printed
# before the child started.
normalize_output() {
    if [ -n "$RUNNER_CONSOLE" ]; then tr -d '\r'; else cat; fi |
    if [ -n "$RUNNER_MARKER" ]; then
        awk -v m="$RUNNER_MARKER" '
            { line[NR] = $0; if (index($0, m)) seen = NR }
            END {
                start = (seen ? seen + 1 : 1)
                for (i = start; i <= NR; i++) print line[i]
            }'
    else cat; fi
}

# Cross targets whose runtime needs post-link instrumentation.  When
# WASIX_ASYNCIFY is set, every linked binary is passed through wasm-opt
# before it is run.
WASM_OPT=${WASM_OPT:-wasm-opt}
WASIX_ASYNCIFY=${WASIX_ASYNCIFY:-}

# Instrument $1 in place.  A no-op when WASIX_ASYNCIFY is unset.
asyncify() {
    [ -n "$WASIX_ASYNCIFY" ] || return 0
    $WASM_OPT $WASIX_ASYNCIFY "$1" -o "$1.opt" 2>/dev/null &&
        mv "$1.opt" "$1"
}

FAIL_CONTEXT=40

echo "run_tests.sh: Received CC=$CC"
echo "run_tests.sh: Running tests on $(uname -s) $(uname -m)"
echo ""


# Check for missing newlines
for file in ./*.c ./*.h; do
    if [ -n "$(tail -c 1 "$file")" ]; then
        echo "✗ Missing newlines in source files"
        exit 1
    fi
done


# Did the build produce a binary?
if [ ! -f "./$PROGNAME" ]; then
    echo "✗ BUILD FAILURE: Binary '$PROGNAME' not found."
    exit 1
fi

# Can it run? Also note the version
VERSION_OUTPUT=$($RUNNER "./$PROGNAME" $RUNNER_SEP --version 2>&1)
rc=$?
VERSION_OUTPUT=$(printf '%s\n' "$VERSION_OUTPUT" | normalize_output)

if [ -n "$RUNNER_CONSOLE" ]; then
    if echo "$VERSION_OUTPUT" | grep -q '^emil '; then
        rc=0
    else
        rc=1
    fi
fi

if [ $rc -ne 0 ]; then
    echo "✗ Binary failed to run (Exit Code $rc)"
    echo "$VERSION_OUTPUT" | tail -20 | sed 's/^/    /'
    exit 1
fi
echo "✓ Binary runs"

BINARY_VERSION=$(echo "$VERSION_OUTPUT" | awk '/emil/ {print $2}')
MAKEFILE_VERSION=$(awk -F'=' '/^VERSION/ {gsub(/[ \t]/, "", $2); print $2}' Makefile)

# Version numbering consistency verification
if [ "$BINARY_VERSION" = "$MAKEFILE_VERSION" ]; then
    echo "✓ Version numbering consistency verified"
elif echo "$BINARY_VERSION" | grep -q "$MAKEFILE_VERSION"; then
    echo "✓ Version numbering consistency verified (Dev build)"
else
    echo "✗ Version numbering mismatch: Binary ($BINARY_VERSION) vs Makefile ($MAKEFILE_VERSION)"
    exit 1
fi

# ===== Source-level invariant checks =====

INVARIANT_FAIL=0

# Mutation-layer invariant: bulkInsert, bulkDelete and pushUndo must
# not appear outside the permitted files.
# Filter out comment lines (/* ... */, // ..., and * continuation lines).
# The grep output has "file.c:NNN:<content>" format, so comment markers
# appear after the second colon.
MUTATION_HITS=$(grep -nE '\b(bulkInsert|bulkDelete|pushUndo)\b' *.c 2>/dev/null \
    | grep -v '^mutate\.c:' \
    | grep -v '^undo\.c:' \
    | grep -v '^buffer\.c:' \
    | grep -vE ':[0-9]+:.*(/\*|//|^[^:]+:[0-9]+:[[:space:]]*\*)' \
    | grep -vE ':[0-9]+:[[:space:]]*\*')
if [ -n "$MUTATION_HITS" ]; then
    echo "✗ Mutation-layer invariant violation:"
    echo "$MUTATION_HITS" | sed 's/^/    /'
    INVARIANT_FAIL=1
else
    echo "✓ Mutation-layer invariant"
fi

# Record-construction invariant: mutate.c is the only place that
# builds undo records.  
RECORD_HITS=$(grep -nE '\bnewUndo\b' *.c 2>/dev/null \
    | grep -v '^mutate\.c:' \
    | grep -v '^undo\.c:[0-9]*:struct undo \*newUndo' \
    | grep -vE ':[0-9]+:.*(/\*|//)' \
    | grep -vE ':[0-9]+:[[:space:]]*\*')
if [ -n "$RECORD_HITS" ]; then
    echo "✗ Record-construction invariant violation (newUndo outside mutate.c):"
    echo "$RECORD_HITS" | sed 's/^/    /'
    INVARIANT_FAIL=1
else
    echo "✓ Record-construction invariant"
fi

# Banned unsafe functions: strcpy, strcat, sprintf, gets, malloc, realloc, calloc
# Use emil_strlcpy/emil_strlcat/snprintf/fgets, xmalloc, xrealloc, xcalloc instead.
# Also getline is not portable; use emil_getline
# All these are defined in util.h
UNSAFE_HITS=$(grep -nE '\b(strcpy|strlcpy|strcat|strlcat|sprintf|gets|getline|malloc|realloc)\s*\(' *.c 2>/dev/null \
    | grep -v '^util.c:' \
    | grep -v 'emil_strlcpy' \
    | grep -v 'emil_strlcat' \
    | grep -v 'snprintf' \
    | grep -v 'fgets' \
    | grep -v 'emil_getline' \
    | grep -v 'xmalloc' \
    | grep -v 'xrealloc' \
    | grep -v 'xcalloc')
if [ -n "$UNSAFE_HITS" ]; then
    echo "✗ Banned unsafe function call:"
    echo "$UNSAFE_HITS" | sed 's/^/    /'
    INVARIANT_FAIL=1
else
    echo "✓ No banned unsafe functions"
fi

# regoff_t signedness invariant: regmatch_t offsets must be read as
# signed, through matchOff() in region.c, before being tested against 0.
REGOFF_HITS=$(grep -nE '\.(rm_so|rm_eo)[[:space:]]*(<|>|<=|>=|==|!=)[[:space:]]*0' *.c 2>/dev/null \
    | grep -vE ':[0-9]+:.*(/\*|//)' \
    | grep -vE ':[0-9]+:[[:space:]]*\*')
if [ -n "$REGOFF_HITS" ]; then
    echo "✗ regoff_t signedness violation (compare matchOff(x), not x):"
    echo "$REGOFF_HITS" | sed 's/^/    /'
    INVARIANT_FAIL=1
else
    echo "✓ regoff_t read as signed"
fi

# Leaf-module invariant: these translation units compute their
# answers from their arguments and must not reach into the global
# editor state. 
LEAF_OBJECTS="abuf.o backup.o base64.o dbuf.o decoder.o \
    emil_subprocess.o history.o transform.o undo.o unicode.o util.o \
    wrap.o"

# Sanity check for nm: keymap.o is known to reference E.  If nm doesn't
# report it, the leaf check is skipped.
LEAF_CONTROL=keymap.o

# Mach-O prefixes symbols with an underscore; ELF does not.
refs_E() {
    $NM -u "$1" 2>/dev/null | grep -qE '^[[:space:]]*U[[:space:]]+_?E$'
}

if [ ! -f "$LEAF_CONTROL" ]; then
    echo "‣ Leaf-module invariant SKIP (objects not present)"
elif ! refs_E "$LEAF_CONTROL"; then
    echo "‣ Leaf-module invariant SKIP (nm cannot resolve E in $LEAF_CONTROL)"
else
    LEAF_HITS=""
    for obj in $LEAF_OBJECTS; do
        if [ ! -f "$obj" ]; then
            LEAF_HITS="$LEAF_HITS
    $obj: not built (listed in LEAF_OBJECTS but absent)"
        elif refs_E "$obj"; then
            LEAF_HITS="$LEAF_HITS
    $obj: references the global E"
        fi
    done
    if [ -n "$LEAF_HITS" ]; then
        echo "✗ Leaf-module invariant violation:"
        echo "$LEAF_HITS" | sed '/^$/d'
        INVARIANT_FAIL=1
    else
        echo "✓ Leaf-module invariant"
    fi
fi

if [ "$INVARIANT_FAIL" -ne 0 ]; then
    echo ""
    echo "Source invariant check failed: fix before proceeding."
    exit 1
fi


# Y2038 safety: emil uses time_t only for mtime equality comparison.
# Ban libc functions that interpret time_t values, since they break
# on 32-bit platforms after 2038-01-19.  Arithmetic and ordering on
# time_t are also unsafe but require semantic analysis to detect;
# those are caught by code review.
Y2038_BANNED="localtime|gmtime|mktime|strftime|difftime|ctime|asctime"
y2038_hits=$(grep -rn -E "\b($Y2038_BANNED)\b" *.c *.h 2>/dev/null | grep -v "^tests/" || true)
if [ -n "$y2038_hits" ]; then
    echo "✗ Y2038 safety violation: banned time function found in source"
    echo "$y2038_hits" | sed 's/^/    /'
    exit 1
fi
echo "✓ Y2038 safe"

echo ""

# ===== Unit test suites (fat binary) =====
#
# Each test binary links every .o except main.o and terminal.o.
# stubs.o provides E, page_overlap, and no-op terminal functions.

ANY_FAIL=0
ANY_WARN=0
BUILD_LOG=$(mktemp)
trap 'rm -f "$BUILD_LOG"' EXIT

# Detect sanitizer build
SANITIZER_FLAGS=""
if nm unicode.o 2>/dev/null | grep -q "__asan_"; then
    echo "Detected sanitizer build"
    SANITIZER_FLAGS="-fsanitize=address,undefined"
fi

# Use the CFLAGS from the Makefile (passed via environment) and add
# test-specific flags. 

# Build stubs.o (replaces main.o + terminal.o)
TEST_CFLAGS="$CFLAGS -I."
$CC $TEST_CFLAGS $SANITIZER_FLAGS -c tests/stubs.c -o tests/stubs.o 2>&1 || {
    echo "✗ Failed to compile stubs.c"
    exit 1
}

# All objects except main.o and terminal.o
TEST_OBJECTS="decoder.o unicode.o buffer.o region.o undo.o transform.o \
    find.o pipe.o register.o fileio.o display.o  keymap.o \
    edit.o prompt.o util.o completion.o history.o base64.o abuf.o \
    window.o ctags.o adjust.o mutate.o wrap.o motion.o dbuf.o \
    emil_subprocess.o palette.o backup.o tests/stubs.o"

# ===== Generated fixture: backup.c against fake syscalls  =====
#
# See tests/make_backup_fixture.sh.
if ! sites=$(sh tests/make_backup_fixture.sh backup.c tests/backup_faked.c); then
    echo "✗ backup.c fixture generation failed"
    exit 1
fi
echo "✓ backup.c rewritten against fake syscalls ($sites call sites)"
echo ""

echo "Unit tests:"

SUITES="decoder unicode wcwidth buffer undo coalesce edit fileio relpath offset
    visual_line utf8_validate rect replace transform subprocess shell adjust
    history abuf tilde keymap kill_ring insert_file status_bar cjk_indic
    warnings ctags find display prompt regex_semantics writeall backup
    fuzz search_nested search_stale register_preview"


# The y2038 CI job (armv7 under QEMU user-mode) sets EMIL_SKIP_QEMU_TESTS.
# Shell completion fails there and the cause is not established.  That
# job exists to show time_t is handled, which these tests do not cover.
if [ -n "${EMIL_SKIP_QEMU_TESTS:-}" ]; then
    SUITES=$(echo "$SUITES" | tr ' ' '\n' | grep -v '^prompt$' | tr '\n' ' ')
    echo "‣ Skipping 'prompt' suite and pty_input_test (EMIL_SKIP_QEMU_TESTS)"
fi

for suite in $SUITES; do
    src="tests/test_${suite}.c"
    bin="tests/test_${suite}"
    printf "  %-16s " "$suite"

    if $CC $TEST_CFLAGS $SANITIZER_FLAGS -o "$bin" "$src" $TEST_OBJECTS \
        $LDFLAGS 2>"$BUILD_LOG"; then
        asyncify "$bin"
    else
        echo "BUILD FAIL"
        tail -20 "$BUILD_LOG" | sed 's/^/    /'
        ANY_FAIL=1
        continue
    fi
    if [ -s "$BUILD_LOG" ]; then
        echo "WARN"
        sed 's/^/    /' "$BUILD_LOG"
        ANY_WARN=1
        printf '  %-12s ' "$suite"
    fi

    if [ -n "$BUILD_ONLY" ]; then
        echo "BUILT (not run)"
        [ -n "$KEEP_BUILT" ] || rm -f "$bin"
        continue
    fi

    # Capture first so rc is the suite's exit status, then normalize.
    output=$($RUNNER "./$bin" 2>&1)
    rc=$?
    output=$(printf '%s\n' "$output" | normalize_output)

    if [ -n "$RUNNER_CONSOLE" ]; then
        if printf '%s\n' "$output" | grep -q '^OK$'; then
            rc=0
        elif printf '%s\n' "$output" | grep -q '^FAIL$'; then
            rc=1
        else
            rc=125   # never reached TEST_END
        fi
    fi

    if [ "$rc" -eq 0 ]; then
        echo "PASS ($(printf '%s\n' "$output" \
            | awk '/Tests/{print $1; exit}') tests)"
    else
        if [ "$rc" -eq 125 ] && [ -n "$RUNNER_CONSOLE" ]; then
            echo "FAIL (did not reach TEST_END)"
        elif [ "$rc" -gt 128 ]; then
            echo "FAIL (killed by signal $((rc - 128)))"
        else
            echo "FAIL (exit $rc)"
        fi
        printf '%s\n' "$output" | head -n "$FAIL_CONTEXT" | sed 's/^/    /'
        ANY_FAIL=1
    fi

    rm -f "$bin"
done

rm -f tests/stubs.o tests/backup_faked.c

# Terminal-level integration: drive the built binary under a pty.
#
#   pty_input_test    needs a pty.  Escape sequences, key batching,
#                     burst detection.  Runs wherever posix_openpt does.
#   pty_signals_test  needs a pty whose master reports the slave's
#                     termios, and real job control.  Linux and macOS.
#
PTY_TESTS="pty_input_test"
case "$(uname -s)" in
Linux|Darwin) PTY_TESTS="$PTY_TESTS pty_signals_test" ;;
NetBSD) PTY_TESTS="" ;;
esac

if [ -n "${EMIL_SKIP_QEMU_TESTS:-}" ]; then
    PTY_TESTS=$(echo "$PTY_TESTS" | sed 's/pty_input_test//')
fi

if [ -z "$RUNNER" ] && [ -x ./emil ]; then
    for t in $PTY_TESTS; do
        printf '  %-16s ' "$t"
        if $CC $TEST_CFLAGS -Itests "tests/$t.c" -o "tests/$t" \
            && "./tests/$t" ./emil > /tmp/pty_out 2>&1; then
            n=$(grep -c '^  .*PASS$' /tmp/pty_out)
            if [ "$n" -eq 0 ]; then
                # Exit 0 having run nothing is not a pass.
                echo "FAIL (no scenarios ran)"
                cat /tmp/pty_out
                ANY_FAIL=1
            else
                echo "PASS ($n scenarios)"
            fi
        else
            echo "FAIL"
            cat /tmp/pty_out
            ANY_FAIL=1
        fi
        rm -f "tests/$t" /tmp/pty_out
    done
fi

# Print the last line of the report
echo ""
echo "-------------------------------------------------------"

if [ "$ANY_FAIL" -ne 0 ]; then
    if [ -n "$BUILD_ONLY" ]; then
        echo "BUILD STATUS: FAILED"
        exit 1
    fi
    echo "TEST STATUS: FAILED"
    exit 1
elif [ "$ANY_WARN" -ne 0 ]; then
    echo "TEST STATUS: ALL PASSED (with compiler warnings)"
    exit 1
else
    echo "TEST STATUS: ALL PASSED"
    exit 0
fi

