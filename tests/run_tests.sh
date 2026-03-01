#!/bin/sh
# Test suite for emil — Strategy C (fat binary)

echo "Running tests..."
echo ""

# ===== Integration tests =====

./emil --version > /dev/null || exit 1
echo "✓ Version check"

MAKEFILE_VERSION=$(grep "^VERSION = " Makefile | cut -d' ' -f3)
BINARY_VERSION=$(./emil --version | awk '{print $NF}')
if [ "$BINARY_VERSION" != "$MAKEFILE_VERSION" ]; then
    if echo "$BINARY_VERSION" | grep -q "$MAKEFILE_VERSION"; then
        echo "✓ Version consistency (Dev build: $BINARY_VERSION)"
    else
        echo "✗ Version mismatch: Binary has '$BINARY_VERSION', Makefile has '$MAKEFILE_VERSION'"
        exit 1
    fi
fi

test -x ./emil || exit 1
echo "✓ Binary executable"
echo ""

# ===== Unit test suites (Strategy C: fat binary) =====
#
# Each test binary links every .o except main.o and terminal.o.
# stubs.o provides E, page_overlap, and no-op terminal functions.

PASS=0
FAIL=0

# Detect sanitizer build
SANITIZER_FLAGS=""
if nm unicode.o 2>/dev/null | grep -q "__asan_"; then
    echo "Detected sanitizer build"
    SANITIZER_FLAGS="-fsanitize=address,undefined"
fi

CFLAGS="-std=c99 -Wall -Wextra -Wpedantic -Wno-pointer-sign -Wno-unused-function -D_DEFAULT_SOURCE -D_BSD_SOURCE -I."

# Build stubs.o (replaces main.o + terminal.o)
cc $CFLAGS $SANITIZER_FLAGS -c tests/stubs.c -o tests/stubs.o 2>&1 || {
    echo "✗ Failed to compile stubs.c"
    exit 1
}

# All objects except main.o and terminal.o
TEST_OBJECTS="wcwidth.o unicode.o buffer.o region.o undo.o transform.o \
    find.o pipe.o register.o fileio.o display.o message.o keymap.o \
    edit.o prompt.o util.o completion.o history.o base64.o abuf.o \
    window.o tests/stubs.o"

echo "Unit tests:"

for suite in unicode wcwidth buffer undo edit fileio; do
    src="tests/test_${suite}.c"
    bin="tests/test_${suite}"
    printf "  %-12s " "$suite"

    # Compile
    if ! cc $CFLAGS $SANITIZER_FLAGS -o "$bin" "$src" $TEST_OBJECTS 2>/dev/null; then
        echo "BUILD FAIL"
        cc $CFLAGS $SANITIZER_FLAGS -o "$bin" "$src" $TEST_OBJECTS 2>&1 | tail -5
        FAIL=$((FAIL+1))
        continue
    fi

    # Run
    output=$(./$bin 2>&1)
    rc=$?

    if [ $rc -gt 128 ]; then
        sig=$((rc - 128))
        echo "CRASH (signal $sig)"
        FAIL=$((FAIL+1))
    elif echo "$output" | grep -q "FAIL"; then
        total=$(echo "$output" | awk '/Tests/{print $1; exit}')
        echo "FAIL ($total tests)"
        echo "$output" | grep "FAIL" | grep -v "^FAIL$" | head -3 | sed 's/^/    /'
        FAIL=$((FAIL+1))
    else
        total=$(echo "$output" | awk '/Tests/{print $1; exit}')
        echo "PASS ($total tests)"
        PASS=$((PASS+1))
    fi

    rm -f "$bin"
done

rm -f tests/stubs.o

echo ""
echo "Suites: $((PASS+FAIL))  Passed: $PASS  Failed: $FAIL"

if [ "$FAIL" -gt 0 ]; then
    exit 1
else
    echo ""
    echo "All tests passed"
fi
