# Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT

# Remember to keep the version number up to date
VERSION = 0.9.9

PROGNAME = emil
PREFIX = /usr/local
DESTDIR =
SHELL = /bin/sh

# Standard C99 compiler settings
CC = cc

# Enable BSD and POSIX features portably
DEFAULT_CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -D_DEFAULT_SOURCE -D_BSD_SOURCE -O2

# The "User" overrides (blank by default)
CFLAGS = 
LDFLAGS = 

# The actual flags used for compilation
ALL_CFLAGS = $(DEFAULT_CFLAGS) $(CFLAGS)

# Installation directories
BINDIR  = $(PREFIX)/bin
MANDIR  = $(PREFIX)/share/man
DOCDIR  = $(PREFIX)/share/doc/emil
LICDIR  = $(PREFIX)/share/licenses/emil

# Source files
OBJECTS = main.o unicode.o decoder.o buffer.o region.o undo.o transform.o \
          backup.o \
          find.o pipe.o register.o fileio.o terminal.o display.o  \
          keymap.o edit.o prompt.o util.o completion.o history.o base64.o \
          abuf.o window.o ctags.o adjust.o mutate.o wrap.o motion.o dbuf.o \
          emil_subprocess.o palette.o

HEADERS = abuf.h adjust.h backup.h base64.h buffer.h completion.h ctags.h \
          dbuf.h decoder.h display.h edit.h emil.h emil_subprocess.h \
          fileio.h find.h history.h keymap.h motion.h mutate.h \
          palette.h pipe.h prompt.h region.h register.h terminal.h \
          transform.h undo.h unicode.h util.h window.h \
          wrap.h

# Default target
all: $(PROGNAME)

# Object files carry no target in their names, so a native build and a
# cross build cannot share a tree: wasm-ld rejects native objects with
# "unknown file type", and the host linker rejects wasm ones with "file
# format not recognized".  Both directions used to require the user to
# remember `make clean`, and the failure appears at link time with a
# message that names neither cause nor cure.
#
# The stamp records which target produced the objects on disk.  When it
# disagrees with the target now being built, the objects are removed
# before anything is compiled.  The recipe writes the file only when the
# tag actually changes, so its mtime -- and therefore any rebuild it
# forces -- stays put across ordinary repeat builds.
BUILD_TAG ?= native

.build-tag: FORCE
	@if [ ! -f $@ ] || [ "`cat $@`" != "$(BUILD_TAG)" ]; then \
		if [ -f $@ ]; then \
			echo "Makefile: build target changed (`cat $@` -> $(BUILD_TAG)); removing objects"; \
			rm -f $(OBJECTS) $(PROGNAME) emil emil.wasm emil.raw.wasm tests/*.wasm; \
		fi; \
		echo "$(BUILD_TAG)" > $@; \
	fi

FORCE:

$(OBJECTS): .build-tag

# Every object depends on every header. emil.h alone reaches 25 of 30
# translation units, and a full build is ~4s, so precise per-object
# dependency tracking would save ~1s at the cost of correctness risk.
$(OBJECTS): $(HEADERS)


# Link the executable
$(PROGNAME): $(OBJECTS)
	$(CC) -o $(PROGNAME) $(OBJECTS) $(LDFLAGS)


# POSIX suffix rule for .c to .o
.SUFFIXES: .c .o
.c.o:
	$(CC) $(ALL_CFLAGS) -DEMIL_VERSION=\"$(VERSION)\" -c $<
#	@echo "CFLAGS: $(ALL_CFLAGS)"

# Installation

install: $(PROGNAME)
	mkdir -p $(DESTDIR)$(BINDIR)
	mkdir -p $(DESTDIR)$(MANDIR)/man1
	mkdir -p $(DESTDIR)$(DOCDIR)
	mkdir -p $(DESTDIR)$(LICDIR)
	cp $(PROGNAME) $(DESTDIR)$(BINDIR)/$(PROGNAME)
	chmod 755 $(DESTDIR)$(BINDIR)/$(PROGNAME)
	cp $(PROGNAME).1 $(DESTDIR)$(MANDIR)/man1/$(PROGNAME).1
	chmod 644 $(DESTDIR)$(MANDIR)/man1/$(PROGNAME).1
	cp README.md $(DESTDIR)$(DOCDIR)/README.md
	chmod 644 $(DESTDIR)$(DOCDIR)/README.md
	cp LICENSE $(DESTDIR)$(LICDIR)/LICENSE
	chmod 644 $(DESTDIR)$(LICDIR)/LICENSE

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(PROGNAME)
	rm -f $(DESTDIR)$(MANDIR)/man1/$(PROGNAME).1
	rm -f $(DESTDIR)$(DOCDIR)/README.md
	rm -f $(DESTDIR)$(LICDIR)/LICENSE
	-rmdir $(DESTDIR)$(DOCDIR) 2>/dev/null
	-rmdir $(DESTDIR)$(LICDIR) 2>/dev/null

# Cleanup
# emil.raw.wasm is the pre-asyncify link output.  The wasix target
# unlinks it only on success, so a failed wasm-opt leaves it behind;
# it is named here so that failure does not leave an untracked artifact.
clean:
	rm -f $(OBJECTS) $(PROGNAME) emil.wasm emil.raw.wasm tests/*.wasm .build-tag

# Testing
test: $(PROGNAME)
	@echo "Makefile: Launching tests with CC=$(CC)"
	@uname -a
	CC="$(CC)" CFLAGS="$(ALL_CFLAGS)" LDFLAGS="$(LDFLAGS)" ./tests/run_tests.sh

check: test


sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -fPIE -DEMIL_DEBUG_ROW_CACHE" \
	        LDFLAGS="-fsanitize=address,undefined -pie" test

# Sorry Dave
HAL_WARNINGS = -Wall -Wextra -Wpedantic \
	-Wduplicated-cond -Wduplicated-branches -Wlogical-op \
	-Wshadow -Wnull-dereference -Wjump-misses-init \
	-Wredundant-decls -Wmissing-prototypes -Wstrict-prototypes \
	-Wold-style-definition -Wswitch-enum -Wvla \
	-Wformat=2 -Wno-format-nonliteral \
	-Wcast-align=strict -Wundef -Wpointer-arith \
	-Wbad-function-cast -Wunused-macros -Wformat-signedness \
	-Wstrict-overflow=2 -Winit-self -Wfloat-equal \
	-Wshift-overflow=2 -Wmissing-declarations \
	-Wwrite-strings -Wcast-qual

hal:
	$(MAKE) format
	$(MAKE) clean
	for f in *.c; do clang-tidy $$f -- -I. ; done
	$(MAKE) CFLAGS="$(CFLAGS) -D_POSIX_C_SOURCE=200112L -D_FORTIFY_SOURCE=3 -Werror" $(PROGNAME)
	$(MAKE) CFLAGS="$(CFLAGS) -D_POSIX_C_SOURCE=200112L -D_FORTIFY_SOURCE=3 -Werror" test
	$(MAKE) clean
	$(MAKE) CC=gcc CFLAGS="$(HAL_WARNINGS) -O2 -Werror" $(PROGNAME)
	@echo "--- gcc -fanalyzer (advisory) ---"
	@for f in *.c; do \
		gcc $(DEFAULT_CFLAGS) -DEMIL_VERSION='"$(VERSION)"' \
		    -fanalyzer -c $$f -o /dev/null 2>&1 || true; \
	done
	$(MAKE) clean
	$(MAKE) CFLAGS="-Werror" test

# Development targets
debug:
	@GIT_VERSION="`git describe --tags --always --dirty 2>/dev/null || echo $(VERSION)`"; \
	$(MAKE) VERSION="$$GIT_VERSION" CFLAGS="$(CFLAGS) -g -O0" $(PROGNAME)

format:
	clang-format -i *.c *.h

# Platform-specific variants (POSIX Compatible)
android:
	$(MAKE) CC=clang \
	CFLAGS="$(CFLAGS) -fPIC -fPIE -DEMIL_DISABLE_SHELL" \
	LDFLAGS="-pie" \
	$(PROGNAME)

msys2:
	$(MAKE) CFLAGS="$(CFLAGS) -D_GNU_SOURCE" $(PROGNAME)

minimal:
	$(MAKE) CFLAGS="$(CFLAGS) -DEMIL_DISABLE_SHELL -Os" $(PROGNAME)

solaris:
	$(MAKE) CC=cc \
	CFLAGS="$(CFLAGS) -xc99 -D__EXTENSIONS__ -O2 -errtags=yes -erroff=E_ARG_INCOMPATIBLE_WITH_ARG_L" \
	$(PROGNAME)

darwin:
	$(MAKE) CC=clang CFLAGS="$(CFLAGS) -D_DARWIN_C_SOURCE" $(PROGNAME)

# WebAssembly via WASIX (https://wasix.org)
#
# Toolchain: wasi-sdk supplies clang and wasm-ld; wasix-libc supplies the
# sysroot.  Point WASI_SDK and WASIX_SYSROOT at unpacked copies of each,
# or run tests/wasix/setup.sh to fetch the pinned versions.
#
# Three details are load-bearing and easy to get wrong:
#
#  1. The clang major version must match the one wasix-libc's sysroot was
#     built with.  A mismatch is not a build error -- it links cleanly and
#     then corrupts memory at run time, because the prebuilt libc keeps
#     path-resolution state in thread-local storage and an older wasm-ld
#     lays that segment out differently.  The symptom is a trap in strcpy
#     inside __wasilibc_find_relpath_alloc on the first open().  See
#     WASIX_CLANG_MAJOR in tests/wasix/setup.sh.
#
#  2. The target and sysroot flags have to be repeated in LDFLAGS.  The
#     link rule uses LDFLAGS but not CFLAGS, so without them the link
#     silently falls back to wasi-sdk's own sysroot and fails on every
#     termios and signal symbol.
#
#  3. Do NOT define _WASI_EMULATED_SIGNAL.  WASIX has real signals and
#     ships no libwasi-emulated-signal.a; the macro only redirects SIG_IGN
#     to a __SIG_IGN that nothing defines.
#
#  4. _WASI_EMULATED_PROCESS_CLOCKS is required; _WASI_EMULATED_MMAN is
#     not, and neither emulation library is linked.
#
#     The clocks macro is a header gate, not a symbol dependency: the
#     sysroot's <sys/resource.h> is a bare #error without it, and
#     pipe.c reaches that header through <sys/wait.h>.  So the shell
#     needs it -- which is new, because pipe.c compiled to a stub under
#     -DEMIL_DISABLE_SHELL and never included <sys/wait.h> at all.
#
#     Nothing needs the mman macro: the tree compiles without it.  And
#     nothing needs either -lwasi-emulated-mman or
#     -lwasi-emulated-process-clocks: emil calls no mmap and no
#     getrusage, so the link resolves with -lc and -lm alone.  Verified
#     by building the whole tree and every suite wasix-test compiles.
#
#     Do not "tidy" the clocks macro away on the grounds that emil
#     calls nothing from it.  That is true and irrelevant; the header
#     refuses to be included either way.
#
#  5. wasm-opt --asyncify is applied uniformly, to emil and to every
#     sandbox binary, and that uniformity is the policy -- not a
#     property of emil's own syscalls.
#
#     The mechanism: wasmer implements proc_fork by snapshotting the
#     module, and the snapshot needs asyncify instrumentation to unwind
#     and rewind the stack.  Without it fork() aborts the process with
#     exit 79 -- the program runs up to the call and the child never
#     appears.  The accompanying thread and TLS flags and the
#     __wasm_init_tls / __wasm_signal exports are part of the same
#     mechanism: omit the exports and the abort merely changes to
#     exit 45.
#
#     That reasoning is load-bearing for the sandbox, where dash forks.
#     It is NOT load-bearing for emil: emil reaches the shell through
#     posix_spawn, which wasmer serves with proc_spawn3, and emil.wasm's
#     import section names proc_spawn3 and no proc_fork.  A
#     non-asyncified emil.wasm has been built and runs correctly,
#     M-! and M-| pipelines included.
#
#     It is applied to emil anyway, deliberately.  The editor spawns
#     binaries that do fork, the instrumentation is cheap, and a build
#     rule that applies to every wasm artifact is one fewer condition to
#     get wrong than a rule that exempts the one artifact whose import
#     list happens to be clean today.  Do not remove it from emil on the
#     grounds that emil does not fork; that is true and is not the
#     reason it is there.
#
#     Recorded because none of this is discoverable from an error
#     message.  The recipe comes from wasix-org/dash's own Makefile.
#
#  6. Suspend needs no flag here.  emil's job-control paths (C-z,
#     C-x z and the C-x C-z shell drawer) are compiled out on
#     __wasi__, in the source, the way __sun selects its headers.
#     wasmer honours SIGTSTP and terminates rather than stopping into
#     a shell that can fg it, so on this runtime suspending cost the
#     user their unsaved buffer.  See dispatchMisc() in keymap.c for
#     why this is a platform test and not a feature test.
#
#  7. -Wno-deprecated is not hiding anything in emil's own code.  It
#     suppresses one clang driver notice, emitted once per translation
#     unit and once at link: --target=wasm32-wasmer-wasi normalises to
#     wasm32-wasi, which clang 22 deprecates in favour of wasm32-wasip1.
#     The triple cannot move -- the sysroot's library directory is
#     lib/wasm32-wasi, so a wasip1 triple fails to find crt1.o, -lc and
#     -lm.  It lives in WASIX_TARGET so that it reaches LDFLAGS as well;
#     when it was only in CFLAGS the link stayed noisy.
#
# -nodefaultlibs with an explicit library list is deliberate: clang looks
# for compiler-rt builtins under a wasm32-wasmer-wasi directory that
# wasi-sdk does not ship, so the sysroot's own copy is named directly.
# This keeps the build hermetic rather than patching the SDK tree.
# Plain '=', not '?=': the conditional assignment operator was not
# standardised until POSIX Issue 8 (2024), and this project targets
# POSIX.1-2001, so Solaris make rejects '?=' outright -- "Badly formed
# macro assignment", before any target is considered.  A command-line
# assignment still overrides these, which is how CI supplies both.
# tests/wasix/setup.sh installs wasi-sdk, the wasix sysroot and
# binaryen side by side under one prefix.  Resolve all three from that
# prefix, so pointing the build at a toolchain configures the whole
# toolchain.  WASM_OPT used to be a bare `wasm-opt` resolved from PATH
# while these two were paths: a CI job that ran the setup script into
# its own prefix and passed WASI_SDK and WASIX_SYSROOT then compiled
# every object and failed on the last command of the build, because the
# third component of the same toolchain was found by a different rule.
WASIX_PREFIX  = $(HOME)/opt
WASI_SDK      = $(WASIX_PREFIX)/wasi-sdk
WASIX_SYSROOT = $(WASIX_PREFIX)/wasix-sysroot/sysroot
WASIX_LIBDIR   = $(WASIX_SYSROOT)/lib/wasm32-wasi
# -pthread/-mthread-model/-ftls-model and the __wasm_init_tls and
# __wasm_signal exports below are what let the runtime snapshot a
# process.  Without them the link fails on __wasm_init_tls (pulled in
# by wasi_thread_start.o); with the flags but not the exports, fork()
# aborts the process at run time instead.  See item 5.
WASIX_TARGET   = --target=wasm32-wasmer-wasi --sysroot=$(WASIX_SYSROOT) \
                 -matomics -mbulk-memory -mmutable-globals \
                 -pthread -mthread-model posix -ftls-model=local-exec \
                 -Wno-deprecated \
                 -D_WASI_EMULATED_PROCESS_CLOCKS
WASIX_EXPORTS  = -Wl,--shared-memory -Wl,--max-memory=4294967296 \
                 -Wl,--import-memory -Wl,--export-dynamic \
                 -Wl,--export=__heap_base -Wl,--export=__stack_pointer \
                 -Wl,--export=__data_end -Wl,--export=__wasm_init_tls \
                 -Wl,--export=__wasm_signal -Wl,--export=__tls_size \
                 -Wl,--export=__tls_align -Wl,--export=__tls_base
WASIX_LIBS     = -nodefaultlibs -lc -lm \
                 $(WASIX_LIBDIR)/libclang_rt.builtins-wasm32.a
# Prefer the copy sitting beside WASI_SDK -- derived from WASI_SDK
# rather than WASIX_PREFIX so that overriding WASI_SDK alone, as CI
# does, still finds it -- and fall back to PATH.
WASM_OPT       = $(firstword \
                   $(wildcard $(dir $(patsubst %/,%,$(WASI_SDK)))binaryen/bin/wasm-opt) \
                   wasm-opt)
WASIX_ASYNCIFY = -O2 --asyncify --enable-threads --enable-bulk-memory \
                 --enable-mutable-globals

# Suites not run under WASIX, and why.  Each reason below was checked
# against wasi-sdk 33 + the pinned wasix-libc sysroot and, where the
# question was behavioural, against wasmer 7.2.1 -- not inferred from
# the sysroot's symbol table.
#
# That distinction is the whole point.  wasix-libc ships fork, vfork,
# the execv family, waitpid, pipe, select, kill, raise, sigaction and
# the whole posix_spawn family, and its libc.imports names proc_fork,
# proc_spawn, proc_join and proc_signal on the runtime side.  An
# earlier version of this comment reasoned from exactly that and
# concluded three of these four suites were deferrals rather than
# gaps.  They are not: a declared symbol that links is not a working
# syscall, and under wasmer fork() terminates the process and
# posix_spawn has nothing to exec.  Do not re-derive this from the
# sysroot; run it.
#
#   warnings -- a real platform gap.  Needs POSIX record locking, and
#     wasix-libc's fcntl.h declares struct flock but none of the locking
#     constants: F_GETLK, F_SETLK, F_SETLKW, the F_RDLCK/F_WRLCK/F_UNLCK
#     l_type values and the F_OFD_* commands all sit inside its
#     __wasilibc_unmodified_upstream guard, which is not defined here.
#     fileio.c guards on the command constants (#if !defined(F_GETLK) ||
#     !defined(F_SETLK)), so the lock is compiled out and the suite has
#     nothing to assert.  The guard tests two of the constants and the
#     other branch needs six, which is sound only because they are all
#     absent together -- verified against the pinned sysroot, and worth
#     rechecking if WASIX_LIBC is bumped.
#
#     There is no OFD-based way out: F_OFD_SETLK is absent too, so an
#     OFD lock would not compile.  Closing this needs wasix-libc to
#     expose the commands and the runtime to implement them.
#
#   subprocess, shell -- run when a sandbox is present; skipped when
#     it is not.  These were long described as unavailable on WASIX.
#     That was wrong: posix_spawn and execve both work, and wasmer will
#     exec a wasm module out of the mapped filesystem.  The sandbox is
#     simply empty by default -- /bin exists, /bin/sh does not -- so
#     spawning a shell fails with ENOEXEC for want of a file rather
#     than a syscall.  tests/wasix/sandbox.sh builds dash and a set of
#     sbase utilities to fill it, after which both suites run.
#
#   writeall -- forks a reader and signals it with SIGUSR1 to force a
#     short write.  fork() works now (item 5), so the suite builds,
#     links and runs -- but it is not reliable here.  Measured over
#     repeated runs: 2 in 3 die with exit 127, "termination signal:
#     User defined signal 1", and the third reports its one test
#     skipped because write() was not interrupted.  wasmer terminates
#     on SIGUSR1 rather than honouring the disposition the test
#     expects, so the suite either aborts or cannot reach its
#     precondition.
#
#     Left skipped for the flakiness, not for the build, which is a
#     different reason from the one recorded here before.  Do not
#     un-skip on the strength of a single green run; that is what
#     happened while writing this.
#
WASIX_SANDBOX  = ./wasix-sandbox

# No -DEMIL_DISABLE_SHELL.  It was passed here for as long as shell
# integration was thought impossible on this target; it is not, so the
# shell is built in.  What a stock WASIX sandbox lacks is a /bin/sh to
# spawn, not a syscall -- see the note above and
# tests/wasix/sandbox.sh, which builds one.
#
# The binary is therefore useful without a sandbox and more useful with
# one: with no /bin/sh, M-! and M-| report the spawn failure and
# nothing else misbehaves.
# BUILD_TAG makes the object-format mismatch self-correcting in both
# directions: see the stamp rule near the top.  This target does not
# clean unconditionally -- it only pays for a rebuild when the previous
# build in this tree was for a different target.
wasix: wasix-toolchain-check
	$(MAKE) BUILD_TAG=wasix CC="$(WASI_SDK)/bin/clang" \
	CFLAGS="$(CFLAGS) $(WASIX_TARGET)" \
	LDFLAGS="$(WASIX_TARGET) $(WASIX_EXPORTS) $(WASIX_LIBS)" \
	PROGNAME=emil.raw.wasm
	$(WASM_OPT) $(WASIX_ASYNCIFY) emil.raw.wasm -o emil.wasm
	@rm -f emil.raw.wasm

# Checked before anything is compiled, not after.  All three components
# are needed for the target to produce output, so finding out about a
# missing one at the last command -- having compiled thirty objects and
# linked them -- wastes the whole build and reads as a build failure
# rather than a setup problem.  Diagnostics go to stderr so they
# interleave with make's own output in the right order; sent to stdout
# they appeared after the sub-make's "Leaving directory" line, which
# put the cause below the effect in CI logs.
.PHONY: wasix-toolchain-check
wasix-toolchain-check:
	@fail=0; \
	[ -x "$(WASI_SDK)/bin/clang" ] || { \
	  echo "Makefile: no clang at $(WASI_SDK)/bin/clang" >&2; fail=1; }; \
	[ -d "$(WASIX_SYSROOT)" ] || { \
	  echo "Makefile: no wasix sysroot at $(WASIX_SYSROOT)" >&2; fail=1; }; \
	command -v $(WASM_OPT) >/dev/null 2>&1 || { \
	  echo "Makefile: no wasm-opt at $(WASM_OPT)" >&2; \
	  echo "  asyncify instrumentation is what makes fork() work under" >&2; \
	  echo "  wasmer; without it the binary aborts at the first fork." >&2; \
	  fail=1; }; \
	[ $$fail -eq 0 ] || { \
	  echo "" >&2; \
	  echo "  Run tests/wasix/setup.sh, which installs all three under one" >&2; \
	  echo "  prefix, then build with WASIX_PREFIX=<that prefix>.  Override" >&2; \
	  echo "  WASI_SDK, WASIX_SYSROOT or WASM_OPT individually if they are" >&2; \
	  echo "  not installed together." >&2; \
	  exit 1; }

wasix-test: wasix
	@echo "Makefile: Launching WASIX tests under wasmer"
	@# Smoke test first, and deliberately.  It is the only thing here
	@# that starts the real binary -- every unit suite links stubs.o in
	@# place of main.o and terminal.o -- and the failure it catches is a
	@# wasi-sdk/wasix-libc mismatch that links without a diagnostic and
	@# then traps on the first open().  Running it after the suites meant
	@# that a single failing suite stopped make before it ever ran, so
	@# the check went missing exactly when the build was suspect.
	./tests/wasix/smoke.sh
	CC="$(WASI_SDK)/bin/clang" \
	CFLAGS="$(DEFAULT_CFLAGS) $(CFLAGS) $(WASIX_TARGET)" \
	LDFLAGS="$(WASIX_TARGET) $(WASIX_EXPORTS) $(WASIX_LIBS)" \
	WASM_OPT="$(WASM_OPT)" \
	WASIX_ASYNCIFY="$(WASIX_ASYNCIFY)" \
	PROGNAME=emil.wasm \
	RUNNER="wasmer run --volume $(PWD):$(PWD) --cwd $(PWD) $(if $(wildcard $(WASIX_SANDBOX)/bin/sh),--volume $(WASIX_SANDBOX)/bin:/bin --env PATH=/bin --env TMPDIR=/tmp,)" \
	RUNNER_SEP="--" \
	NM="$(WASI_SDK)/bin/llvm-nm" \
	./tests/run_tests.sh


# Redox OS (https://redox-os.org)
#
# A Unix-like system written in Rust, with its own C library, relibc.
# The Redox project publishes a prebuilt GCC cross compiler and a
# matching relibc sysroot; tests/redox_setup.sh fetches both into one
# prefix, which is what REDOX_TOOLCHAIN points at.
#
# No target-specific CFLAGS.  That is worth stating, because every other
# non-Unix target here needs something: Android and Genode compile out
# the shell, Solaris needs __EXTENSIONS__, WASIX needs a sysroot flag and
# a post-link pass.  Redox needs none of it -- relibc supplies the whole
# POSIX surface this editor uses, including posix_spawn and the pty
# family, and the stock C99 flags are enough.
#
# BUILD_TAG is not optional here, and matters more than it does for
# wasix.  A Redox object and a native Linux object are both x86-64 ELF,
# so nothing rejects a mixture: in a tree that was last built natively,
# `make CC=<redox-gcc>` finds every .o newer than its source, recompiles
# nothing, relinks nothing, and exits 0 leaving the native binary in
# place.  The wasm mismatch at least announces itself.  This one is
# silent, and a job that trusted the exit status would report Redox as
# building fine having never invoked the cross compiler.
REDOX_TARGET ?= x86_64-unknown-redox
REDOX_TOOLCHAIN ?= $(HOME)/opt/redox-toolchain
REDOX_CC = $(REDOX_TOOLCHAIN)/bin/$(REDOX_TARGET)-gcc
REDOX_NM = $(REDOX_TOOLCHAIN)/bin/$(REDOX_TARGET)-nm

redox: redox-toolchain-check
	$(MAKE) BUILD_TAG=redox CC="$(REDOX_CC)" $(PROGNAME)

# Runs the unit suites inside a real Redox system, via redoxer, which
# boots Redox under QEMU and execs a binary in it.  Each suite is one
# boot, so this is minutes rather than seconds.
#
# RUNNER_CONSOLE is not optional here: redoxer does not reliably carry
# the child's exit status back out of the VM.  See the block that
# defines it in tests/run_tests.sh.
REDOXER ?= redoxer

redox-run: redox
	@echo "Makefile: running the suites inside Redox via $(REDOXER)"
	CC="$(REDOX_CC)" \
	CFLAGS="$(DEFAULT_CFLAGS) $(CFLAGS)" \
	LDFLAGS="$(LDFLAGS)" \
	NM="$(REDOX_NM)" \
	RUNNER="$(REDOXER) exec --folder . --" \
	RUNNER_CONSOLE=1 \
	RUNNER_MARKER="## running redoxer ##" \
	./tests/run_tests.sh

# Checked before anything is compiled, for the reason given above the
# wasix equivalent: finding out about a missing sysroot after thirty
# objects have compiled reads as a build failure rather than a setup
# problem.
.PHONY: redox-toolchain-check
redox-toolchain-check:
	@fail=0; \
	[ -x "$(REDOX_CC)" ] || { \
	  echo "Makefile: no Redox compiler at $(REDOX_CC)" >&2; fail=1; }; \
	[ -d "$(REDOX_TOOLCHAIN)/$(REDOX_TARGET)/include" ] || { \
	  echo "Makefile: no relibc sysroot at $(REDOX_TOOLCHAIN)/$(REDOX_TARGET)" >&2; \
	  echo "  gcc-install.tar.gz on its own is not enough -- it has the" >&2; \
	  echo "  compiler but no headers.  relibc-install.tar.gz carries the" >&2; \
	  echo "  sysroot, and both unpack into the same prefix." >&2; \
	  fail=1; }; \
	[ $$fail -eq 0 ] || { \
	  echo "" >&2; \
	  echo "  Run tests/redox_setup.sh <prefix>, then build with" >&2; \
	  echo "  REDOX_TOOLCHAIN=<prefix>/redox-toolchain." >&2; \
	  exit 1; }

# Help
help:
	@echo "emil build targets:"
	@echo "  all       Build emil (default)"
	@echo "  install   Install to PREFIX ($(PREFIX))"
	@echo "  uninstall Remove installed files"
	@echo "  clean     Remove object files"
	@echo "  test      Run basic test"
	@echo "  debug     Build with debug symbols"
	@echo "  android   Build for Android/Termux"
	@echo "  darwin    Build for macOS/Darwin"
	@echo "  msys2     Build for MSYS2"
	@echo "  minimal   Build minimal version"
	@echo "  solaris   Build for Solaris Developer Studio"
	@echo "  wasix     Build emil.wasm for WASIX (see tests/wasix/setup.sh)"
	@echo "  wasix-test Build and test emil.wasm under wasmer"
	@echo "  redox     Cross-build for Redox OS (see tests/redox_setup.sh)"
	@echo "  redox-run Run the suites inside Redox via redoxer"
	@echo "  check     Alias for test"
	@echo "  format    Format code with clang-format"
	@echo "  hal       HAL-9000 compliance"

.PHONY: FORCE wasix-toolchain-check redox-toolchain-check all install uninstall clean test check sanitize hal debug format android msys2 minimal solaris darwin wasix wasix-test redox redox-run help

# Terminal-level integration tests on their own; run_tests.sh runs them
# too, and decides there which of the two this platform can host.
test-pty: $(PROGNAME)
	$(CC) $(ALL_CFLAGS) -Itests -o tests/pty_input_test tests/pty_input_test.c
	./tests/pty_input_test ./$(PROGNAME)
	$(CC) $(ALL_CFLAGS) -Itests -o tests/pty_signals_test tests/pty_signals_test.c
	./tests/pty_signals_test ./$(PROGNAME)
	rm -f tests/pty_input_test tests/pty_signals_test

