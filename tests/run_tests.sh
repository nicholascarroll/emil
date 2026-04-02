#!/bin/sh
set -u

COMPILER=${CC:-cc}
CFLAGS=${CFLAGS:-""}
LDFLAGS=${LDFLAGS:-""}

echo "run_tests.sh: Received COMPILER=$COMPILER"
echo "run_tests.sh: Running tests on $(uname -s) $(uname -m)"
echo ""

# Did the build produce a binary?
if [ ! -f "./emil" ]; then
    echo "✗ BUILD FAILURE: Binary 'emil' not found."
    exit 1
fi

# Can it run? Also note the version
VERSION_OUTPUT=$(./emil --version 2>&1)
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


# ===== Unit test suites (fat binary) =====
#
# Each test binary links every .o except main.o and terminal.o.
# stubs.o provides E, page_overlap, and no-op terminal functions.

ANY_FAIL=0

# Detect sanitizer build
SANITIZER_FLAGS=""
if nm unicode.o 2>/dev/null | grep -q "__asan_"; then
    echo "Detected sanitizer build"
    SANITIZER_FLAGS="-fsanitize=address,undefined"
fi

CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Wno-pointer-sign -Wno-unused-function -D_DEFAULT_SOURCE -D_BSD_SOURCE -I."

# Build stubs.o (replaces main.o + terminal.o)


$COMPILER $CFLAGS $SANITIZER_FLAGS -c tests/stubs.c -o tests/stubs.o 2>&1 || {
    echo "✗ Failed to compile stubs.c"
    exit 1
}

# All objects except main.o and terminal.o
TEST_OBJECTS="wcwidth.o unicode.o buffer.o region.o undo.o transform.o \
    find.o pipe.o register.o fileio.o display.o message.o keymap.o \
    edit.o prompt.o util.o completion.o history.o base64.o abuf.o \
    window.o clang.o adjust.o tests/stubs.o"

echo "Unit tests:"

for suite in unicode wcwidth buffer undo edit fileio relpath visual_line utf8_validate rect_undo transform; do
    src="tests/test_${suite}.c"
    bin="tests/test_${suite}"
    printf "  %-12s " "$suite"

    # Compile
    if ! $COMPILER $CFLAGS $SANITIZER_FLAGS -o "$bin" "$src" $TEST_OBJECTS 2>/dev/null; then
        echo "BUILD FAIL"
        $COMPILER $CFLAGS $SANITIZER_FLAGS -o "$bin" "$src" $TEST_OBJECTS 2>&1 | tail -5
        ANY_FAIL=$((FAIL+1))
        continue
    fi

    # Run
    output=$(./$bin 2>&1)
    rc=$?

    if [ $rc -gt 128 ]; then
        echo "CRASH (signal $((rc - 128)))"
        ANY_FAIL=1
    elif echo "$output" | grep -q "FAIL"; then
        # Defect 1 Fix: No test count on failure
        echo "FAIL" 
        echo "$output" | grep "FAIL:" | head -n 3 | sed 's/^/    /'
        ANY_FAIL=1
    elif [ $rc -ne 0 ]; then
        echo "FAIL (Sanitizer/Error - Exit Code $rc)"
        # REMOVED 'head -n 5' to show the full report
        echo "$output" | grep -iE "AddressSanitizer|LEAK|ERROR" -A 20 2>/dev/null | sed 's/^/    /'
        ANY_FAIL=1
    else
        # Success is the only place we report the test count
        total=$(echo "$output" | awk '/Tests/{print $1; exit}')
        echo "PASS ($total tests)"
    fi

    rm -f "$bin"
done

rm -f tests/stubs.o


# Print the last line of the report
echo ""
echo "-------------------------------------------------------"

if [ "$ANY_FAIL" -ne 0 ]; then
    echo "TEST STATUS: FAILED"
    exit 1
else
    echo "TEST STATUS: ALL PASSED"
    exit 0
fi

