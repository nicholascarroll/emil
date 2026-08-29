#!/bin/sh
# Build a WASIX sandbox root containing /bin/sh and the utilities the
# shell suites need, so `make wasix-test` can exercise shell integration
# instead of skipping it.
#
# Usage: ./tests/wasix/sandbox.sh [prefix] [outdir]
#   prefix  where setup.sh put wasi-sdk and the sysroot ($HOME/opt)
#   outdir  where to write the sandbox bin/ directory (./wasix-sandbox)
#
# Then:  wasmer run emil.wasm --dir . --volume <outdir>/bin:/bin \
#              --env PATH=/bin -- FILE
#
# --- Why each step is here ------------------------------------------
#
# A WASIX sandbox starts empty.  wasmer will happily exec a wasm module
# out of the mapped filesystem -- posix_spawn("/bin/sh", ...) and
# execve() both work -- but nothing puts a shell there.  That is the
# whole reason shell integration was previously written off as
# unavailable on this target: not a missing syscall, a missing file.
#
# Three build settings are load-bearing and none of them is obvious:
#
#  1. wasm-opt --asyncify.  This is what makes fork() work.  wasmer
#     implements proc_fork by snapshotting the module, which needs
#     asyncify instrumentation to unwind and rewind the stack.  Without
#     it fork() aborts the process with exit 79 -- the program prints up
#     to the call and the child never runs.  A shell without fork cannot
#     run a pipeline.
#
#  2. The thread/TLS flags (-pthread -mthread-model posix
#     -ftls-model=local-exec).  Without them the link fails on
#     __wasm_init_tls, pulled in by wasi_thread_start.o.
#
#  3. The linker exports below, __wasm_init_tls and __wasm_signal in
#     particular.  The runtime calls them when it restores a snapshot.
#     Omitting them turns the exit-79 abort into an exit-45 abort, which
#     looks like a different bug and is not.
#
# This recipe is taken from wasix-org/dash's own Makefile, which is the
# only place it appears to be written down.
set -eu

PREFIX=${1:-$HOME/opt}
OUT=${2:-./wasix-sandbox}

WASI_SDK=${WASI_SDK:-$PREFIX/wasi-sdk}
WASIX_SYSROOT=${WASIX_SYSROOT:-$PREFIX/wasix-sysroot/sysroot}
CC="$WASI_SDK/bin/clang"
WASM_OPT=${WASM_OPT:-wasm-opt}

# Pinned to commits so a rebuild is reproducible.  Neither project
# tags releases, so these are commit SHAs rather than tags; codeload
# serves a tarball for a SHA at the same URL shape it serves one for a
# branch.  An earlier version of this file said "pinned so a rebuild is
# reproducible" while naming the master branch of both, which is the
# opposite of pinned: the sandbox a bisect built would depend on the
# day it ran.  Override to track upstream deliberately.
DASH_REF=${DASH_REF:-6da4b3b3db2d9f7de9c34a8b3650e63c11360249}
SBASE_REF=${SBASE_REF:-b30fb56804bfed69b45ef0e944d2e029e4d26258}

for t in "$CC" ; do
    [ -x "$t" ] || { echo "✗ not found: $t (run tests/wasix/setup.sh)" >&2; exit 1; }
done
command -v "$WASM_OPT" >/dev/null 2>&1 || {
    echo "✗ wasm-opt not on PATH.  Install binaryen:" >&2
    echo "  https://github.com/WebAssembly/binaryen/releases" >&2
    exit 1
}

TARGET="--target=wasm32-wasmer-wasi --sysroot=$WASIX_SYSROOT \
 -matomics -mbulk-memory -mmutable-globals \
 -pthread -mthread-model posix -ftls-model=local-exec \
 -Wno-deprecated -D_WASI_EMULATED_MMAN -D_WASI_EMULATED_PROCESS_CLOCKS"

EXPORTS="-Wl,--shared-memory -Wl,--max-memory=4294967296 \
 -Wl,--import-memory -Wl,--export-dynamic \
 -Wl,--export=__heap_base -Wl,--export=__stack_pointer \
 -Wl,--export=__data_end -Wl,--export=__wasm_init_tls \
 -Wl,--export=__wasm_signal -Wl,--export=__tls_size \
 -Wl,--export=__tls_align -Wl,--export=__tls_base"

LIBS="-nodefaultlibs -lc -lm -lwasi-emulated-mman \
 -lwasi-emulated-process-clocks \
 $WASIX_SYSROOT/lib/wasm32-wasi/libclang_rt.builtins-wasm32.a"

ASYNCIFY="-O2 --asyncify --enable-threads --enable-bulk-memory \
 --enable-mutable-globals"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT/bin"

# --- dash, as /bin/sh -----------------------------------------------
echo "==> fetching dash ($DASH_REF)"
curl -fsSL -o "$WORK/dash.tar.gz" \
    "https://codeload.github.com/wasix-org/dash/tar.gz/$DASH_REF"
mkdir -p "$WORK/dash"
tar xzf "$WORK/dash.tar.gz" -C "$WORK/dash" --strip-components=1

echo "==> building dash"
( cd "$WORK/dash/src"
  # token_vars.h is generated and not checked in; mktokens is a POSIX
  # sh script and runs on the build host, not the target.
  sh ./mktokens
  DASH_SRC="alias arith_yacc arith_yylex builtins cd error eval exec \
expand histedit init input jobs mail main memalloc miscbltin mystring \
nodes output options parser redir show signames syntax system trap var"

  # printf, test and times exist twice in the dash tree: src/NAME.c is
  # the standalone program and bltin/NAME.c is the shell builtin.  They
  # differ in that bltin/bltin.h remaps putchar, printf, fputs and
  # friends onto dash's own output layer (out1/out1c/out1fmt) -- but
  # only under -DSHELL, which upstream supplies from Makefile.am and
  # this fork's wasm Makefile does not.
  #
  # Building src/printf.c without -DSHELL therefore compiles, links and
  # runs, and writes through libc stdio instead.  dash leaves the shell
  # via _exit(), so that buffer is never flushed and every byte is
  # silently lost: `printf '\007'` produced nothing while `echo`, `pwd`
  # and `type` -- which call out1 directly -- worked.  Nothing reports
  # an error; the builtin just goes quiet.
  #
  # emil's test_pipe_cancel_escalates_to_sigkill is what caught this:
  # it delivers C-g as `printf '\007'`, so the cancellation was never
  # seen and the suite blamed SIGKILL.
  #
  # The two variants are not interchangeable.  src/*.c define their own
  # error()/info() and re-include memalloc.h and error.h, so they fail
  # to compile *with* -DSHELL.  Build the bltin/ copies, with -DSHELL,
  # as upstream does.
  DASH_BLTIN="printf test times"

  # shellcheck disable=SC2086
  for s in $DASH_SRC; do
      $CC $TARGET -O2 -include ../config.h -fno-trapping-math \
          -Wno-everything -c "$s.c" -o "$s.o"
  done
  # shellcheck disable=SC2086
  for s in $DASH_BLTIN; do
      $CC $TARGET -O2 -include ../config.h -fno-trapping-math \
          -DSHELL -I. -Wno-everything -c "bltin/$s.c" -o "$s.o"
  done
  OBJ=""
  for s in $DASH_SRC $DASH_BLTIN; do OBJ="$OBJ $s.o"; done
  # shellcheck disable=SC2086
  $CC $TARGET $EXPORTS -o dash.raw.wasm $OBJ $LIBS
  # shellcheck disable=SC2086
  $WASM_OPT $ASYNCIFY dash.raw.wasm -o dash.wasm )
cp "$WORK/dash/src/dash.wasm" "$OUT/bin/sh"

# --- sbase, for the utilities ---------------------------------------
#
# suckless sbase rather than GNU coreutils/busybox: it is plain C with a
# flat Makefile and no autotools, configure, or Linux headers, so it
# cross-compiles to wasm without patching.  Its sed is POSIX and is what
# the shell suites and the README examples actually exercise.
#
# sbase ships no diff; that comes from OpenBSD below.
echo "==> fetching sbase ($SBASE_REF)"
curl -fsSL -o "$WORK/sbase.tar.gz" \
    "https://codeload.github.com/michaelforney/sbase/tar.gz/$SBASE_REF"
mkdir -p "$WORK/sbase"
tar xzf "$WORK/sbase.tar.gz" -C "$WORK/sbase" --strip-components=1

# dd is not decoration: tests/test_shell.c uses `dd bs=1 count=10` for
# its early-exiting child, because `head -c` is a GNU/BSD extension and
# sbase's POSIX head rejects it.
TOOLS="sed cat echo printf head tail sort tr wc grep rev tee uniq cut \
true false yes seq sleep od dd"

echo "==> building sbase tools"
( cd "$WORK/sbase"
  for f in libutf/*.c libutil/*.c; do
      $CC $TARGET -O2 -D_DEFAULT_SOURCE -D_BSD_SOURCE -Wno-everything \
          -I. -c "$f" -o "${f%.c}.o"
  done
  # shellcheck disable=SC2086
  for t in $TOOLS; do
      $CC $TARGET -O2 -D_DEFAULT_SOURCE -D_BSD_SOURCE -Wno-everything -I. \
          $EXPORTS -o "$t.raw.wasm" "$t.c" libutf/*.o libutil/*.o $LIBS
      # shellcheck disable=SC2086
      $WASM_OPT $ASYNCIFY "$t.raw.wasm" -o "$t.wasm"
  done )
for t in $TOOLS; do cp "$WORK/sbase/$t.wasm" "$OUT/bin/$t"; done

# --- diff, from OpenBSD ---------------------------------------------
#
# emil shells out to `diff -u` for M-x diff-buffer-with-file and keys on
# its exit status (0 same, 1 differs, >=2 error), so a real diff is
# needed, not a stub.  OpenBSD's is self-contained C -- no autotools, no
# gnulib -- which is why it is here rather than GNU diffutils.
#
# Three small accommodations, all in third-party code, none in emil:
#   __dead        an OpenBSD attribute spelling; dropped.
#   unveil/pledge OpenBSD sandboxing with no WASI analogue.  Removed:
#                 the runtime already confines the process to its
#                 mapped filesystem.
#   err/errx/warn/warnx/warnc
#                 wasix-libc declares <err.h> but implements none of it,
#                 so the shim below supplies the five diff calls.
echo "==> fetching and building diff (OpenBSD)"
DIFF_URL=https://raw.githubusercontent.com/openbsd/src/master/usr.bin/diff
mkdir -p "$WORK/diff"
( cd "$WORK/diff"
  for f in diff.c diff.h diffdir.c diffreg.c xmalloc.c xmalloc.h; do
      curl -fsSL -o "$f" "$DIFF_URL/$f"
  done

  # __dead is a declaration attribute, not a type; just remove it.
  sed -i.bak 's/^__dead void usage(void);/void usage(void);/; s/^__dead void$/void/' diff.c
  # Drop the unveil/pledge block (6 lines starting at the first unveil).
  sed -i.bak '/if (unveil("\/tmp", "rwc") == -1)/,+5d' diff.c

  # wasix-libc's <err.h> declares no warnc, so diffdir.c would declare
  # it implicitly as int-returning and wasm-ld would warn about the
  # signature mismatch against the shim.  Give it a real prototype.
  printf '%s\n' 'void warnc(int, const char *, ...);' >> diff.h

  cat > err_shim.c <<'SHIM'
#include <err.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static void vout(int use_errno, const char *fmt, va_list ap) {
	int e = errno;
	fputs("diff: ", stderr);
	if (fmt) vfprintf(stderr, fmt, ap);
	if (use_errno) fprintf(stderr, "%s%s", fmt ? ": " : "", strerror(e));
	fputc('\n', stderr);
}
void err(int c, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vout(1, fmt, ap); va_end(ap); exit(c); }
void errx(int c, const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vout(0, fmt, ap); va_end(ap); exit(c); }
void warn(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vout(1, fmt, ap); va_end(ap); }
void warnx(const char *fmt, ...)
{ va_list ap; va_start(ap, fmt); vout(0, fmt, ap); va_end(ap); }
void warnc(int code, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	fputs("diff: ", stderr); vfprintf(stderr, fmt, ap);
	fprintf(stderr, ": %s\n", strerror(code)); va_end(ap);
}
SHIM

  for f in diff.c diffdir.c diffreg.c xmalloc.c err_shim.c; do
      $CC $TARGET -O2 -D_DEFAULT_SOURCE -D_BSD_SOURCE -D_GNU_SOURCE \
          -Wno-everything -I. -c "$f" -o "${f%.c}.o"
  done
  # shellcheck disable=SC2086
  $CC $TARGET $EXPORTS -o diff.raw.wasm \
      diff.o diffdir.o diffreg.o xmalloc.o err_shim.o $LIBS
  # shellcheck disable=SC2086
  $WASM_OPT $ASYNCIFY diff.raw.wasm -o diff.wasm )
cp "$WORK/diff/diff.wasm" "$OUT/bin/diff"

chmod +x "$OUT/bin"/*

# --- verify -----------------------------------------------------------
#
# A builtin that writes to the wrong stdout produces no output and no
# error (see the -DSHELL note above), so a sandbox can look complete and
# still be useless.  Check the shell can emit a byte through each of the
# paths the suites depend on, rather than trusting that it linked.
echo "==> verifying sandbox"
sb_fail=0
sb_check() { # label, expected-hex, command
    got=$(wasmer run "$OUT/bin/sh" --volume "$OUT/bin:/bin" --env PATH=/bin \
              -- -c "$3" 2>/dev/null | od -An -tx1 | tr -d ' \n')
    if [ "$got" = "$2" ]; then
        echo "  ok   $1"
    else
        echo "  FAIL $1: expected [$2] got [$got]" >&2
        sb_fail=1
    fi
}
if command -v wasmer >/dev/null 2>&1; then
    sb_check "echo (builtin)"        "68690a"   'echo hi'
    sb_check "printf (builtin)"      "07"       "printf '\\007'"
    sb_check "printf %s (builtin)"   "6869"     'printf %s hi'
    sb_check "external utility"      "68690a"   '/bin/echo hi'
    sb_check "pipeline (needs fork)" "48490a"   'echo hi | tr a-z A-Z'
    sb_check "dd (test_shell.c)"     "3031"     'printf 0123 | dd bs=1 count=2 2>/dev/null'
    [ "$sb_fail" = 0 ] || { echo "✗ sandbox is not usable" >&2; exit 1; }
else
    echo "  wasmer not on PATH -- skipping verification"
fi

echo ""
echo "WASIX sandbox ready in $OUT"
echo "  $(ls "$OUT/bin" | wc -l | tr -d ' ') binaries, including sh and sed"
echo ""
echo "  wasmer run emil.wasm --dir . --volume $OUT/bin:/bin \\"
echo "        --env PATH=/bin --env TMPDIR=/tmp -- FILE"
