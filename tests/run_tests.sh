#!/bin/sh
set -u

CC=${CC:-cc}
CFLAGS=${CFLAGS:-""}
LDFLAGS=${LDFLAGS:-""}

# Cross-target support.  All three default to the native behaviour, so
# an ordinary `make test` runs exactly as it did before.
#
#   PROGNAME     name of the built editor binary (the wasix target
#                builds emil.wasm rather than emil).
#   RUNNER       command prefix used to execute a built binary.  Empty
#                natively; for a Wasm target this is the host runtime,
#                e.g. RUNNER="wasmer run --dir .".  Binaries are always
#                invoked with a leading ./ so wasmer treats the argument
#                as a path rather than a registry package name.
#   SKIP_SUITES  space-separated suites to skip, for platforms that
#                cannot build or run them.
#   RUNNER_SEP   separator inserted between the binary and its own
#                arguments.  wasmer needs a literal -- there, or it
#                consumes flags like --version itself.
PROGNAME=${PROGNAME:-emil}
RUNNER=${RUNNER:-}
RUNNER_SEP=${RUNNER_SEP:-}
SKIP_SUITES=${SKIP_SUITES:-}

# How many individual failure lines to print per suite.  The failure
# *count* is always reported in full; this caps only the detail.  0
# means print everything.
MAX_FAIL_LINES=${MAX_FAIL_LINES:-10}

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

if [ $rc -ne 0 ]; then
    echo "✗ Binary failed to run (Exit Code $rc)"
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
#
# These grep the editor source (not tests/) for patterns that must
# not appear.  Catches mistakes before they reach a runtime test.

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

# Record-construction invariant (#104): mutate.c is the only place that
# builds undo records.  undo.c stores, merges and replays them; it must
# not allocate one.  This is what keeps a virtual-EOF fix like #102
# from having to be written twice.
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

if [ "$INVARIANT_FAIL" -ne 0 ]; then
    echo ""
    echo "Source invariant check failed — fix before proceeding."
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

# ===== Generated fixture: backup.c against fake syscalls (#125) =====
#
# backup.c carries no test hooks and no test-only macros; the editor
# source is what ships.  Fault injection is done by copying it here and
# rewriting its syscall names to the fk_* fakes that tests/test_backup.c
# defines, so the suite drives the real control flow over a filesystem
# that fails on demand.
#
# makeVerifiedBackup is renamed too.  test_backup.c #includes this copy
# rather than linking it -- that is what makes the file-local helpers
# reachable -- and the real backup.o is in TEST_OBJECTS, so without the
# rename the two definitions would collide at link time.
#
# The substitution is deliberately blunt, so it is checked rather than
# trusted: the expected number of call sites is asserted below, and a
# drift in either direction fails the run instead of silently testing
# the real filesystem.  BRE with an explicit leading-character class,
# not \b, because \b is a GNU extension that BSD and Solaris sed lack.
BACKUP_SYSCALLS="open close read write fsync unlink fcntl"
BACKUP_EXPECTED_SITES=22

sed_script=""
for fn in $BACKUP_SYSCALLS; do
    sed_script="$sed_script -e s/\\([^a-zA-Z0-9_]\\)$fn(/\\1fk_$fn(/g"
done
sed_script="$sed_script -e s/makeVerifiedBackup/fake_makeVerifiedBackup/g"

# shellcheck disable=SC2086
sed $sed_script backup.c > tests/backup_faked.c

sites=$(grep -c 'fk_' tests/backup_faked.c)
if [ "$sites" -ne "$BACKUP_EXPECTED_SITES" ]; then
    echo "✗ backup.c syscall rewrite: expected $BACKUP_EXPECTED_SITES call"
    echo "  sites, rewrote $sites.  backup.c changed shape -- update"
    echo "  BACKUP_EXPECTED_SITES in tests/run_tests.sh after checking"
    echo "  tests/test_backup.c still fakes every syscall it now uses."
    exit 1
fi
if grep -q '[^a-zA-Z0-9_]makeVerifiedBackup(' tests/backup_faked.c; then
    echo "✗ backup.c rewrite left an un-renamed makeVerifiedBackup"
    exit 1
fi
echo "✓ backup.c rewritten against fake syscalls ($sites call sites)"
echo ""

echo "Unit tests:"

# Suites are listed explicitly rather than globbed, so adding a file is
# a deliberate act.  The count check below catches the matching mistake:
# a new tests/test_*.c that nobody added here would otherwise be silently
# skipped rather than reported.
SUITES="decoder unicode wcwidth buffer undo coalesce edit fileio relpath offset
    visual_line utf8_validate rect replace transform subprocess shell adjust
    history abuf tilde keymap kill_ring insert_file status_bar cjk_indic
    warnings ctags find display prompt regex_semantics writeall backup"

listed=$(echo $SUITES | wc -w)
present=$(ls tests/test_*.c 2>/dev/null | wc -l)
if [ "$listed" -ne "$present" ]; then
    echo "  ERROR: $present tests/test_*.c files but $listed listed in SUITES."
    echo "         A suite is present but unlisted (or listed but missing)."
    for f in tests/test_*.c; do
        nm=$(basename "$f" .c); nm=${nm#test_}
        # Word-split $SUITES rather than glob-match it: the list spans
        # several lines, so a substring match on " $nm " misses names
        # sitting next to a newline or indentation.
        found=0
        for s in $SUITES; do
            [ "$s" = "$nm" ] && found=1
        done
        [ "$found" -eq 1 ] || echo "         unlisted: $f"
    done
    ANY_FAIL=1
fi

for suite in $SUITES; do
    src="tests/test_${suite}.c"
    bin="tests/test_${suite}"
    printf "  %-12s " "$suite"

    skip_this=0
    for skipped in $SKIP_SUITES; do
        if [ "$suite" = "$skipped" ]; then
            skip_this=1
            break
        fi
    done
    if [ "$skip_this" -eq 1 ]; then
        echo "SKIP (unsupported on this target)"
        continue
    fi

    # Compile and link (use TEST_CFLAGS for the test source, LDFLAGS for
    # linking).  Diagnostics are kept rather than sent to /dev/null: a
    # warning in test code was invisible for as long as it was discarded,
    # which is how a suite accumulates them unnoticed.
    if ! $CC $TEST_CFLAGS $SANITIZER_FLAGS -o "$bin" "$src" $TEST_OBJECTS \
        $LDFLAGS 2>"$BUILD_LOG"; then
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

    # Run
    output=$($RUNNER "./$bin" 2>&1)
    rc=$?

    if [ $rc -gt 128 ]; then
        echo "CRASH (signal $((rc - 128)))"
        echo "$output" | grep -E ">>|run_shell|run_command|write_temp" | head -n 5 | sed 's/^/    /'
        ANY_FAIL=1
    elif echo "$output" | grep -q "FAIL"; then
        # Report the true count, then show at most MAX_FAIL_LINES of
        # detail.  The count and the detail are separate: capping the
        # detail keeps the summary readable, but capping the count
        # silently understates how much is broken.  Set
        # MAX_FAIL_LINES=0 for no cap.
        nfail=$(echo "$output" | grep -c "FAIL:")
        if [ "$MAX_FAIL_LINES" -eq 0 ] || [ "$nfail" -le "$MAX_FAIL_LINES" ]; then
            echo "FAIL ($nfail failures)"
            echo "$output" | grep "FAIL:" | sed 's/^/    /'
        else
            echo "FAIL ($nfail failures)"
            echo "$output" | grep "FAIL:" | head -n "$MAX_FAIL_LINES" \
                | sed 's/^/    /'
            echo "    ... $((nfail - MAX_FAIL_LINES)) more (MAX_FAIL_LINES=$MAX_FAIL_LINES)"
        fi
        ANY_FAIL=1
    elif [ $rc -ne 0 ]; then
        echo "FAIL (Sanitizer/Error - Exit Code $rc)"
        # REMOVED 'head -n 5' to show the full report
        echo "$output" | grep -iE "runtime error|AddressSanitizer|LEAK|ERROR" -A 5 2>/dev/null | head -20 | sed 's/^/    /'
        ANY_FAIL=1
    else
        # Success is the only place we report the test count
        total=$(echo "$output" | awk '/Tests/{print $1; exit}')
        skipped=$(echo "$output" | awk '/Tests/{print $5; exit}')
        # A test that skips itself where the platform cannot host it
        # (no hard links, no pty) must not read as a pass.  Show the
        # count so a suite cannot quietly shrink.
        if [ -n "$skipped" ] && [ "$skipped" -gt 0 ] 2>/dev/null; then
            echo "PASS ($total tests, $skipped skipped)"
            echo "$output" | grep '^  SKIP:' | sed 's/^/    /'
        else
            echo "PASS ($total tests)"
        fi
    fi


    rm -f "$bin"
done

# Invariant fuzzer: random command sequences through the mutation and
# undo layers, checking after every operation that the buffer is still
# well-formed and that undoing everything restores the original text.
# Cheap enough to run every time -- 10k sequences is ~0.2s on a plain
# build and ~1.1s under sanitizers -- and it guards exactly the
# invariants the mutation layer is built around.  FUZZ_SEQS=0 skips it.
FUZZ_SEQS="${FUZZ_SEQS:-10000}"
# One pinned seed is one walk through the state space, and a green run
# says only that this walk is clean.  A heap-buffer-overflow in
# clampCursorToViewport lived here for as long as it did because seed 1
# happens not to reach it while roughly half of its neighbours do.
# Run a fixed set instead, so a regression has to survive several
# independent walks to stay hidden, and keep the set fixed rather than
# time-seeded: a suite that fails only on some days is worse than one
# that misses a bug, because nobody trusts it.  FUZZ_SEEDS overrides
# for a deeper soak (`FUZZ_SEEDS="$(seq 1 200)" make test`).
FUZZ_SEEDS="${FUZZ_SEEDS:-${FUZZ_SEED:-1 2 3 4 5 6 7 8}}"
if [ "$FUZZ_SEQS" -gt 0 ]; then
    printf '%-14s ' "  fuzz_undo"
    if ! $CC $TEST_CFLAGS -Itests $SANITIZER_FLAGS -o tests/fuzz_undo \
        tests/fuzz_undo.c $TEST_OBJECTS $LDFLAGS 2>/dev/null; then
        echo "BUILD FAIL"
        $CC $TEST_CFLAGS -Itests $SANITIZER_FLAGS -o tests/fuzz_undo \
            tests/fuzz_undo.c $TEST_OBJECTS $LDFLAGS 2>&1 | tail -5
        ANY_FAIL=1
    else
        fuzz_failed=0
        fuzz_nseeds=0
        for fuzz_seed in $FUZZ_SEEDS; do
            fuzz_nseeds=$((fuzz_nseeds + 1))
            fuzz_out=$($RUNNER ./tests/fuzz_undo $RUNNER_SEP "$FUZZ_SEQS" "$fuzz_seed" 2>&1)
            fuzz_rc=$?
            # The fuzzer reports "N failure(s)" and exits non-zero on any.
            if [ $fuzz_rc -ne 0 ] ||
               ! echo "$fuzz_out" | grep -q "^0 failure"; then
                if [ $fuzz_failed -eq 0 ]; then
                    echo "FAIL"
                fi
                echo "    seed $fuzz_seed:"
                echo "$fuzz_out" | tail -20 | sed 's/^/      /'
                fuzz_failed=1
                ANY_FAIL=1
            fi
        done
        if [ $fuzz_failed -eq 0 ]; then
            echo "PASS ($FUZZ_SEQS sequences x $fuzz_nseeds seeds)"
        fi
    fi
    rm -f tests/fuzz_undo
fi

rm -f tests/stubs.o tests/backup_faked.c

# Terminal-level integration: drive the built binary under a pty.
# decoder_pty_test skips itself (exit 0) if no pty is available.
if [ -z "$RUNNER" ] && [ -x ./emil ]; then
    printf '%-14s ' "  decoder_pty"
    if $CC $TEST_CFLAGS tests/decoder_pty_test.c -o tests/decoder_pty_test \
        && ./tests/decoder_pty_test ./emil > /tmp/pty_test_out 2>&1; then
        pty_scenarios=$(grep -c '^  .*PASS$' /tmp/pty_test_out)
        pty_skipped=$(grep -c '^  .*SKIP (' /tmp/pty_test_out)
        # Individual scenarios skip themselves where the platform
        # cannot observe what they assert on -- illumos ptys are
        # STREAMS devices whose master reports no termios of the
        # slave.  Surface the count rather than let a suite quietly
        # shrink.
        if [ "$pty_skipped" -gt 0 ]; then
            echo "PASS ($pty_scenarios scenarios, $pty_skipped skipped)"
        else
            echo "PASS ($pty_scenarios scenarios)"
        fi
    else
        echo "FAIL"
        cat /tmp/pty_test_out
        ANY_FAIL=1
    fi
    rm -f tests/decoder_pty_test /tmp/pty_test_out
elif [ -n "$RUNNER" ]; then
    # decoder_pty_test is a host program that allocates a pty and execs
    # the editor.  Under a Wasm runtime the editor is not directly
    # executable, so this scenario is covered by tests/wasix_smoke.sh
    # instead, which drives emil.wasm through the runtime under a pty.
    echo "  decoder_pty   SKIP (cross target; see tests/wasix_smoke.sh)"
else
    echo "  decoder_pty   SKIP (emil binary not built)"
fi

# Print the last line of the report
echo ""
echo "-------------------------------------------------------"

if [ "$ANY_FAIL" -ne 0 ]; then
    echo "TEST STATUS: FAILED"
    exit 1
elif [ "$ANY_WARN" -ne 0 ]; then
    # Not fatal here: the portability matrix builds with compilers that
    # each warn about different things.  make hal compiles the tests with
    # -Werror, which is where a warning has to be fixed.
    echo "TEST STATUS: ALL PASSED (with compiler warnings)"
    exit 1
else
    echo "TEST STATUS: ALL PASSED"
    exit 0
fi

