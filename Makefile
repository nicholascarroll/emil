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
          find.o pipe.o register.o fileio.o terminal.o display.o  \
          keymap.o edit.o prompt.o util.o completion.o history.o base64.o \
          abuf.o window.o ctags.o adjust.o mutate.o wrap.o motion.o dbuf.o \
          emil_subprocess.o palette.o

HEADERS = abuf.h adjust.h base64.h buffer.h completion.h ctags.h \
          dbuf.h decoder.h display.h edit.h emil.h emil_subprocess.h \
          fileio.h find.h history.h keymap.h motion.h mutate.h \
          palette.h pipe.h prompt.h region.h register.h terminal.h \
          transform.h undo.h unicode.h util.h window.h \
          wrap.h

# Default target
all: $(PROGNAME)

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
clean:
	rm -f $(OBJECTS) $(PROGNAME) emil.wasm tests/*.wasm

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
# or run tests/wasix_setup.sh to fetch the pinned versions.
#
# Three details are load-bearing and easy to get wrong:
#
#  1. The clang major version must match the one wasix-libc's sysroot was
#     built with.  A mismatch is not a build error -- it links cleanly and
#     then corrupts memory at run time, because the prebuilt libc keeps
#     path-resolution state in thread-local storage and an older wasm-ld
#     lays that segment out differently.  The symptom is a trap in strcpy
#     inside __wasilibc_find_relpath_alloc on the first open().  See
#     WASIX_CLANG_MAJOR in tests/wasix_setup.sh.
#
#  2. The target and sysroot flags have to be repeated in LDFLAGS.  The
#     link rule uses LDFLAGS but not CFLAGS, so without them the link
#     silently falls back to wasi-sdk's own sysroot and fails on every
#     termios and signal symbol.
#
#  3. Do NOT define _WASI_EMULATED_SIGNAL.  WASIX has real signals and
#     ships no libwasi-emulated-signal.a; the macro only redirects SIG_IGN
#     to a __SIG_IGN that nothing defines.  The mman and process-clocks
#     emulation libraries do exist and are used.
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
WASI_SDK      = $(HOME)/opt/wasi-sdk
WASIX_SYSROOT = $(HOME)/opt/wasix-sysroot/sysroot
WASIX_LIBDIR   = $(WASIX_SYSROOT)/lib/wasm32-wasi
WASIX_TARGET   = --target=wasm32-wasmer-wasi --sysroot=$(WASIX_SYSROOT) \
                 -matomics -mbulk-memory -mmutable-globals
WASIX_LIBS     = -nodefaultlibs -lc -lm \
                 -lwasi-emulated-mman -lwasi-emulated-process-clocks \
                 $(WASIX_LIBDIR)/libclang_rt.builtins-wasm32.a

# Suites not run under WASIX.  These were previously all described as
# platform gaps; that is true of one of them.  Stated accurately here,
# because the earlier text reasoned about wasi-libc while this target
# builds against wasix-libc, and the two differ in exactly the places
# the reasoning depended on.  wasix-libc's sysroot ships fork, vfork,
# the execv family, waitpid, pipe, select, kill, raise, sigaction and
# the whole posix_spawn family, and its libc.imports names proc_fork,
# proc_spawn, proc_join and proc_signal on the runtime side.
#
#   warnings -- a real platform gap.  Needs POSIX record locking, and
#     wasix-libc's fcntl.h defines struct flock and F_RDLCK/F_WRLCK/
#     F_UNLCK but not the F_GETLK/F_SETLK commands.  fileio.c already
#     guards on exactly that (#if !defined(F_GETLK) || !defined(F_SETLK)),
#     so the lock is compiled out and the suite has nothing to assert.
#     Note F_OFD_SETLK *is* defined; whether the runtime honours it is
#     untested, and an OFD-based lock would be the way to close this.
#
#   subprocess, shell -- NOT a platform gap.  They are skipped because
#     this target passes -DEMIL_DISABLE_SHELL below, which compiles the
#     shell drawer out.  That flag is a deferral, not a necessity: the
#     symbols emil_subprocess.c needs are all present in the sysroot.
#     Removing it is unfinished work, not an impossibility.
#
#   writeall -- forks a reader and signals it to force a short write.
#     The suite does not link under this toolchain: it drags in
#     wasi_thread_start.o and fails on __wasm_init_tls.  That is a
#     toolchain/threading-model problem, not an absence of fork or of
#     signals -- WASIX has both, which is also why item 3 above says
#     not to define _WASI_EMULATED_SIGNAL.  The property under test
#     (writeAll() loops over a short write) is reachable on this
#     target; it is the harness that does not build.
WASIX_SKIP_SUITES = subprocess shell warnings writeall

# -DEMIL_DISABLE_SHELL here is a deferral, not a platform requirement.
# WASIX supplies posix_spawn, fork and exec, so the shell drawer could
# build; it has not been exercised under a Wasm runtime, so it is left
# out rather than shipped untested.  See WASIX_SKIP_SUITES above.
wasix:
	$(MAKE) CC="$(WASI_SDK)/bin/clang" \
	CFLAGS="$(CFLAGS) $(WASIX_TARGET) -Wno-deprecated \
	-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_PROCESS_CLOCKS \
	-DEMIL_DISABLE_SHELL" \
	LDFLAGS="$(WASIX_TARGET) $(WASIX_LIBS)" \
	PROGNAME=emil.wasm

wasix-test: wasix
	@echo "Makefile: Launching WASIX tests under wasmer"
	CC="$(WASI_SDK)/bin/clang" \
	CFLAGS="$(DEFAULT_CFLAGS) $(CFLAGS) $(WASIX_TARGET) -Wno-deprecated \
	-D_WASI_EMULATED_MMAN -D_WASI_EMULATED_PROCESS_CLOCKS \
	-DEMIL_DISABLE_SHELL" \
	LDFLAGS="$(WASIX_TARGET) $(WASIX_LIBS)" \
	PROGNAME=emil.wasm \
	RUNNER="wasmer run --dir ." \
	RUNNER_SEP="--" \
	SKIP_SUITES="$(WASIX_SKIP_SUITES)" \
	./tests/run_tests.sh
	./tests/wasix_smoke.sh


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
	@echo "  wasix     Build emil.wasm for WASIX (see tests/wasix_setup.sh)"
	@echo "  wasix-test Build and test emil.wasm under wasmer"
	@echo "  check     Alias for test"
	@echo "  format    Format code with clang-format"
	@echo "  hal       HAL-9000 compliance"

.PHONY: all install uninstall clean test check sanitize hal debug format android msys2 minimal solaris darwin wasix wasix-test help

# Terminal-level integration tests: drives the real binary under a
# pseudo-terminal (also run at the end of `make test`).
test-pty: $(PROGNAME)
	$(CC) $(ALL_CFLAGS) -o tests/decoder_pty_test tests/decoder_pty_test.c
	./tests/decoder_pty_test ./$(PROGNAME)
	rm -f tests/decoder_pty_test

