#!/bin/sh
# Test suite for emil
set -e

echo "Running tests..."

# Test 1: Version flag works
./emil --version > /dev/null || exit 1
echo "✓ Version check"


# Test 2: Version consistency
MAKEFILE_VERSION=$(grep "^VERSION = " Makefile | cut -d' ' -f3)
BINARY_VERSION=$(./emil --version | awk '{print $NF}') # Assuming emil --version outputs "emil 0.1.0"

if [ "$BINARY_VERSION" != "$MAKEFILE_VERSION" ]; then
    # If they don't match, check if it's just because it's a Git dev build
    if echo "$BINARY_VERSION" | grep -q "$MAKEFILE_VERSION"; then
        echo "✓ Version consistency (Dev build: $BINARY_VERSION)"
    else
        echo "✗ Version mismatch: Binary has '$BINARY_VERSION', Makefile has '$MAKEFILE_VERSION'"
        exit 1
    fi
fi

# Test 3: Binary exists and is executable  
test -x ./emil || exit 1
echo "✓ Binary executable"

# Test 4: Compile and run core tests
# Check if object files were built with sanitizers by looking for ASAN symbols
if nm unicode.o 2>/dev/null | grep -q "__asan_"; then
    echo "✓ Detected sanitizer build, using sanitizer flags for test"
    cc -std=c99 -fsanitize=address,undefined -o test_core tests/test_core.c unicode.o wcwidth.o util.o || exit 1
else
    cc -std=c99 -o test_core tests/test_core.c unicode.o wcwidth.o util.o || exit 1
fi
if ./test_core | grep -q "FAIL"; then
    echo "✗ Core tests failed"
    ./test_core
    exit 1
else
    echo "✓ Core functionality"
fi
rm -f test_core


echo ""
echo "All tests passed"