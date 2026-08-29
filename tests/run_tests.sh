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
#   RUNNER_SEP   separator inserted between the binary and its own
#                arguments.  wasmer needs a literal -- there, or it
#                consumes flags like --version itself.
#   NM           nm to use for the leaf-module invariant.  The host nm
#                cannot read wasm objects, so a cross build must supply
#                its own, e.g. NM=$WASI_SDK/bin/llvm-nm.
#   BUILD_ONLY   compile and link everything, run none of it.  One
#                caller: tests/asterinas/build_payload.sh, which builds
#                the suites here and stages them into an initramfs to be
#                run inside the VM.  It is the build half of a target
#                that builds and runs, not a way to report a target as
#                working without testing it -- there is no longer a
#                verdict line for that, and no CI job uses this flag.
#   KEEP_BUILT   with BUILD_ONLY, leave the linked suites on disk
#                instead of deleting each one after it links, so they
#                can be staged.  No effect without BUILD_ONLY.
PROGNAME=${PROGNAME:-emil}
RUNNER=${RUNNER:-}
RUNNER_SEP=${RUNNER_SEP:-}
NM=${NM:-nm}
BUILD_ONLY=${BUILD_ONLY:-}
KEEP_BUILT=${KEEP_BUILT:-}

# Runners that speak through a virtual machine's console rather than
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
# This is not defensive vagueness.  redoxer boots Redox under QEMU and
# signals the child's status by having the guest write a magic value to
# a debug-exit port; when the guest powers down before that write lands,
# QEMU exits 0 and redoxer reports a failure it cannot describe.
# Measured at 4 runs in 10 on one suite, with the suite printing correct
# output and a clean summary in 10 runs out of 10.  Trusting $? there
# would fail a green suite about 40% of the time, and the first instinct
# on seeing that is to distrust the editor.
#
# Taking the verdict from the summary is not a workaround so much as the
# stronger check: a binary that dies partway prints no summary at all,
# which is caught here explicitly, whereas an exit status can be lost in
# either direction.
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
# before it is run: wasmer implements fork() by snapshotting the module,
# which needs asyncify instrumentation, and an uninstrumented binary
# aborts at the first fork rather than failing a test.
WASM_OPT=${WASM_OPT:-wasm-opt}
WASIX_ASYNCIFY=${WASIX_ASYNCIFY:-}

# Instrument $1 in place.  A no-op when WASIX_ASYNCIFY is unset, which
# is every native build.
asyncify() {
    [ -n "$WASIX_ASYNCIFY" ] || return 0
    $WASM_OPT $WASIX_ASYNCIFY "$1" -o "$1.opt" 2>/dev/null &&
        mv "$1.opt" "$1"
}

# How a suite is scored, here and in the Genode and Asterinas runners:
# by its exit status, and by nothing else.
#
# Each suite already decides this for itself.  TEST_END() prints the
# counts, prints OK or FAIL, and returns non-zero on any failure or on a
# run of zero tests, and every main() ends with `return TEST_END()`.  So
# the status is the verdict, already computed by the code that has the
# numbers in hand.
#
# There was a scorer that re-derived it by grepping stdout for FAIL:.
# That is how the bug above survived: with the verdict taken from the
# text, rc being permanently 0 changed nothing visible, so nothing failed
# loudly enough to be noticed.  Grep the output to *show* it; never to
# decide it.
#
# The failing suite's own output is printed verbatim.  It already
# contains the FAIL: lines, the per-case names and the summary, so there
# is nothing to reformat and no count to recompute.
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
# Captured before normalizing: $? after a pipeline is the last
# command's status, so piping straight into normalize_output made rc 0
# here too -- and this rc gates an exit 1, so a binary that could not
# run at all was reported as running.
VERSION_OUTPUT=$($RUNNER "./$PROGNAME" $RUNNER_SEP --version 2>&1)
rc=$?
VERSION_OUTPUT=$(printf '%s\n' "$VERSION_OUTPUT" | normalize_output)

# Under a console runner the status above is noise; the binary either
# printed its version or it did not.
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

# regoff_t signedness invariant: regmatch_t offsets must be read as
# signed, through matchOff() in region.c, before being tested against 0.
#
# POSIX requires regoff_t to be signed, and regexec marks a group that
# did not participate in a match by setting rm_so and rm_eo to -1.
# Redox's relibc declares it as size_t, so a bare `rm_so >= 0` is
# always true and the sentinel is never seen.  Nothing here catches
# that: the guard is dead only on relibc, and even there the mistake
# is silent, because relibc sets both fields together and the bad
# length comes out 0.  A grep is the whole defence.
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

# Leaf-module invariant (§2.1): these translation units compute their
# answers from their arguments and must not reach into the global
# editor state.  wrap.c is the one the design document names, because
# that boundary eroded once already (#117) and the review that caught
# it had nothing mechanical to point at.  The rest are here for the
# same reason: each is a leaf today, and a leaf stops being one the
# first time somebody reaches for E to save passing a parameter.
#
# Checked against the objects rather than the source, so a reference
# inside a macro expansion or a #ifdef branch counts and one inside a
# comment does not.  main.o is excluded: it defines E.
LEAF_OBJECTS="abuf.o backup.o base64.o dbuf.o decoder.o \
    emil_subprocess.o history.o transform.o undo.o unicode.o util.o \
    wrap.o"

# Positive control.  A cross-target nm that cannot read these objects
# would report every one of them clean, and the check would pass by
# doing nothing.  keymap.o reads E all over dispatch, so if nm cannot
# see the reference there, nm is not telling us anything.
#
# The control is why this is worth keeping rather than trusting: on the
# WASIX target the host nm reports "file format not recognized" for
# every object, so the invariant used to skip silently.  $NM lets that
# build pass wasi-sdk's llvm-nm, which reads wasm objects and reports
# the same "U E" the control expects.
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
# See tests/make_backup_fixture.sh.  The Genode cross build needs the
# same file before goa starts and cannot call this script, so the
# generation lives in one place that both invoke.
if ! sites=$(sh tests/make_backup_fixture.sh backup.c tests/backup_faked.c); then
    echo "✗ backup.c fixture generation failed"
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
    warnings ctags find display prompt regex_semantics writeall backup
    fuzz"

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
    printf "  %-16s " "$suite"

    # Compile and link (use TEST_CFLAGS for the test source, LDFLAGS for
    # linking).  Diagnostics are kept rather than sent to /dev/null: a
    # warning in test code was invisible for as long as it was discarded,
    # which is how a suite accumulates them unnoticed.
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
        # Kept only when something downstream will run them: the
        # Asterinas job stages these into an initramfs.  Deleting by
        # default keeps a build-only run from leaving binaries that
        # cannot execute on this host lying around the tree.
        [ -n "$KEEP_BUILT" ] || rm -f "$bin"
        continue
    fi

    # Run.
    #
    # Captured first, normalized second.  $? after a pipeline is the
    # LAST command's status, and normalize_output is tr/awk/cat, which
    # always succeed -- so writing this as one pipeline made rc 0 for
    # every suite on every target, and both branches below that read it
    # went dead.  A suite killed by a signal, or aborted by a sanitizer,
    # printed no FAIL: line and fell through to the success branch.
    # Introduced by 622ed1d as part of a Redox CRLF fix; it disabled
    # crash detection on Linux, macOS and the BSDs at the same time.
    output=$($RUNNER "./$bin" 2>&1)
    rc=$?
    output=$(printf '%s\n' "$output" | normalize_output)

    # One runner cannot carry a status out, and only one: redoxer boots
    # Redox under QEMU and signals the child's status by having the guest
    # write to a debug-exit port, so when the guest powers down before
    # that write lands, QEMU exits 0 and the status is lost.  There the
    # suite's own OK/FAIL line stands in for it.  A suite that died
    # partway printed neither, which is the third case.
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
        # Display only.  The count is worth showing so a suite cannot
        # quietly shrink, but it is read back out of the output rather
        # than trusted for anything.
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
# Two binaries, because there are two requirements:
#
#   pty_input_test    needs a pty.  Escape sequences, key batching,
#                     burst detection.  Runs wherever posix_openpt does.
#   pty_signals_test  needs a pty whose master reports the slave's
#                     termios, and real job control.  Linux and macOS.
#
# That split is the whole of the platform handling.  It replaces a
# runtime probe, three environment variables and a skip facility inside
# the C, all of which existed because the two requirements were in one
# binary and the weaker scenarios had to be excused wherever the
# stronger ones could not run.
#
# Both need to execute the editor here, so neither runs under a RUNNER.
PTY_TESTS="pty_input_test"
case "$(uname -s)" in
Linux|Darwin) PTY_TESTS="$PTY_TESTS pty_signals_test" ;;
esac

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
    # Not fatal here: the portability matrix builds with compilers that
    # each warn about different things.  make hal compiles the tests with
    # -Werror, which is where a warning has to be fixed.
    echo "TEST STATUS: ALL PASSED (with compiler warnings)"
    exit 1
else
    echo "TEST STATUS: ALL PASSED"
    exit 0
fi

