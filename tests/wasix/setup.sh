#!/bin/sh
# Fetch the pinned WASIX toolchain into $PREFIX (default $HOME/opt).
#
# Usage:  ./tests/wasix/setup.sh [prefix]
# Then:   make wasix WASI_SDK=<prefix>/wasi-sdk \
#                    WASIX_SYSROOT=<prefix>/wasix-sysroot/sysroot
#
# Everything here is version-pinned on purpose.  wasi-sdk and wasix-libc
# are released independently, and a mismatch between them does not fail
# the build -- it produces a binary that traps on the first file open.
# See the WASIX section of the Makefile for why.
set -eu

PREFIX=${1:-$HOME/opt}

# --- pinned versions -------------------------------------------------
#
# WASIX_CLANG_MAJOR must track the clang that wasix-libc built its
# sysroot with.  wasi-sdk 33 ships clang 22; wasi-sdk 25 ships clang 19
# and miscompiles this sysroot's thread-local storage layout.  If you
# bump WASIX_LIBC, re-run tests/wasix/smoke.sh before trusting the
# result: that test exists to catch exactly this failure.
WASI_SDK_VERSION=33
WASI_SDK_RELEASE=33.0
WASIX_CLANG_MAJOR=22
WASIX_LIBC=v2026-07-30.1
WASMER_VERSION=7.2.1

# binaryen, for wasm-opt.  Not optional: `make wasix` pipes the linked
# module through `wasm-opt --asyncify`, because wasmer implements
# proc_fork by snapshotting the module and the snapshot needs asyncify
# instrumentation.  An uninstrumented binary aborts at the first fork().
BINARYEN_VERSION=123

ARCH=$(uname -m)
case "$ARCH" in
    x86_64|amd64) SDK_ARCH=x86_64 ;;
    aarch64|arm64) SDK_ARCH=arm64 ;;
    *) echo "unsupported architecture: $ARCH" >&2; exit 1 ;;
esac

OS=$(uname -s)
case "$OS" in
    Linux) SDK_OS=linux ;;
    Darwin) SDK_OS=macos ;;
    *) echo "unsupported OS: $OS" >&2; exit 1 ;;
esac

mkdir -p "$PREFIX"
cd "$PREFIX"

SDK_TARBALL="wasi-sdk-${WASI_SDK_RELEASE}-${SDK_ARCH}-${SDK_OS}.tar.gz"
SDK_URL="https://github.com/WebAssembly/wasi-sdk/releases/download/wasi-sdk-${WASI_SDK_VERSION}/${SDK_TARBALL}"

if [ ! -x "$PREFIX/wasi-sdk/bin/clang" ]; then
    echo "==> fetching wasi-sdk ${WASI_SDK_RELEASE}"
    curl -fsSL -o "$SDK_TARBALL" "$SDK_URL"
    tar xzf "$SDK_TARBALL"
    rm -rf wasi-sdk
    mv "wasi-sdk-${WASI_SDK_RELEASE}-${SDK_ARCH}-${SDK_OS}" wasi-sdk
    rm -f "$SDK_TARBALL"

    # Drop the parts of wasi-sdk this build never touches, which is most
    # of it: 646 MB unpacked becomes about 136 MB, and that is what a CI
    # cache has to store and restore on every run.
    #
    # share/wasi-sysroot is the largest single item and the most clearly
    # redundant -- it is wasi-sdk's own wasip1/wasip2 sysroot, while we
    # build against the WASIX one.  clang.cfg still names it as a default
    # --sysroot, which is harmless because every invocation passes
    # --sysroot explicitly.
    #
    # Removing rather than copying a whitelist is deliberate: a whitelist
    # silently loses a file the toolchain starts needing after a version
    # bump, whereas an unexpected new component here simply survives.
    # Set WASIX_KEEP_FULL_SDK=1 to skip this.
    if [ -z "${WASIX_KEEP_FULL_SDK:-}" ]; then
        rm -rf wasi-sdk/share/wasi-sysroot \
               wasi-sdk/share/cmake \
               wasi-sdk/share/man \
               wasi-sdk/lib/cmake \
               wasi-sdk/lib/liblldb* \
               wasi-sdk/lib/libclang.so* \
               wasi-sdk/bin/clang-tidy \
               wasi-sdk/bin/lldb* \
               wasi-sdk/bin/wasm-component-ld
    fi
else
    echo "==> wasi-sdk already present"
fi

# Fail loudly rather than let a silently-miscompiling clang through.
GOT_MAJOR=$("$PREFIX/wasi-sdk/bin/clang" -dumpversion | cut -d. -f1)
if [ "$GOT_MAJOR" != "$WASIX_CLANG_MAJOR" ]; then
    echo "✗ clang major version is $GOT_MAJOR, expected $WASIX_CLANG_MAJOR." >&2
    echo "  wasix-libc's prebuilt sysroot keeps path-resolution state in" >&2
    echo "  thread-local storage; a different wasm-ld lays that segment out" >&2
    echo "  differently and the result traps on the first open()." >&2
    exit 1
fi

if [ ! -d "$PREFIX/wasix-sysroot/sysroot" ]; then
    echo "==> fetching wasix-libc sysroot ${WASIX_LIBC}"
    curl -fsSL -o sysroot.tar.gz \
        "https://github.com/wasix-org/wasix-libc/releases/download/${WASIX_LIBC}/sysroot.tar.gz"
    tar xzf sysroot.tar.gz
    rm -f sysroot.tar.gz
else
    echo "==> wasix sysroot already present"
fi

if ! command -v wasmer >/dev/null 2>&1 && [ ! -x "$HOME/.wasmer/bin/wasmer" ]; then
    echo "==> installing wasmer ${WASMER_VERSION}"
    curl -fsSL https://get.wasmer.io | sh -s -- "v${WASMER_VERSION}"
else
    echo "==> wasmer already present"
fi

if ! command -v wasm-opt >/dev/null 2>&1 && \
   [ ! -x "$PREFIX/binaryen/bin/wasm-opt" ]; then
    echo "==> fetching binaryen ${BINARYEN_VERSION}"
    # binaryen names its assets <arch>-linux / <arch>-macos, not by
    # `uname -s`.  "$ARCH-$OS" happens to resolve on Linux only because
    # GitHub matches asset names case-insensitively, so x86_64-Linux
    # finds x86_64-linux; on macOS it asks for x86_64-Darwin and 404s.
    # The arch spellings do match uname -m: x86_64 and aarch64 on
    # Linux, arm64 on macOS.
    case "$OS" in
        Linux) BINARYEN_OS=linux ;;
        Darwin) BINARYEN_OS=macos ;;
        *) echo "unsupported OS for binaryen: $OS" >&2; exit 1 ;;
    esac
    BINARYEN_TARBALL="binaryen-version_${BINARYEN_VERSION}-${ARCH}-${BINARYEN_OS}.tar.gz"
    curl -fsSL -o "$BINARYEN_TARBALL" \
        "https://github.com/WebAssembly/binaryen/releases/download/version_${BINARYEN_VERSION}/${BINARYEN_TARBALL}"
    tar xzf "$BINARYEN_TARBALL"
    rm -rf "$PREFIX/binaryen"
    mv "binaryen-version_${BINARYEN_VERSION}" "$PREFIX/binaryen"
    rm -f "$BINARYEN_TARBALL"
else
    echo "==> wasm-opt already present"
fi

echo ""
echo "WASIX toolchain ready in $PREFIX"
if ! command -v wasm-opt >/dev/null 2>&1; then
    echo "  wasm-opt is in $PREFIX/binaryen/bin -- add it to PATH, or pass"
    echo "  WASM_OPT=$PREFIX/binaryen/bin/wasm-opt to make."
fi
echo "  make wasix-test WASI_SDK=$PREFIX/wasi-sdk WASIX_SYSROOT=$PREFIX/wasix-sysroot/sysroot"
