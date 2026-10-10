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



# Rebuild when switching between native and cross-compilation targets.
# Native and WASM object files cannot be used interchangeably.
BUILD_TAG = native

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
	$(MAKE) CFLAGS="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -fPIE -DEMIL_DEBUG_ROW_CACHE -DEMIL_DEBUG_FOCUS" \
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
# Toolchain: wasi-sdk (clang, wasm-ld) and wasix-libc (sysroot).
# Set WASI_SDK and WASIX_SYSROOT, or run tests/wasix/setup.sh to fetch pinned versions.
#
# Build constraints:
# - Clang major version must match the wasix-libc sysroot (see WASIX_CLANG_MAJOR in setup.sh).
#   A mismatch links cleanly but causes runtime memory corruption due to TLS layout differences.
# - Target and sysroot flags must be repeated in LDFLAGS. The link rule does not use CFLAGS;
#   omitting them silently falls back to the wasi-sdk sysroot, failing on termios/signal symbols.
# - Do not define _WASI_EMULATED_SIGNAL. WASIX provides real signals; this macro redirects
#   SIG_IGN to an undefined __SIG_IGN.
# - _WASI_EMULATED_PROCESS_CLOCKS is required. It acts as a header gate for <sys/resource.h>
#   (included via <sys/wait.h> in pipe.c). _WASI_EMULATED_MMAN is not required.
# - wasm-opt --asyncify is applied uniformly to all WASM artifacts. While emil uses posix_spawn
#   (proc_spawn3) and does not strictly require it, the sandbox (dash) uses proc_fork, which
#   requires asyncify for stack snapshotting in wasmer. Uniform application prevents build rule fragility.
# - Suspend (SIGTSTP) requires no special flag. Job-control paths are compiled out for __wasi__.
# - -Wno-deprecated suppresses clang 22 warnings when normalizing wasm32-wasmer-wasi to
#   wasm32-wasi. The triple cannot be updated to wasm32-wasip1 due to sysroot library paths.
# - -nodefaultlibs with an explicit library list ensures hermetic builds, bypassing wasi-sdk's
#   missing compiler-rt builtins directory for this target.
WASIX_PREFIX  = $(HOME)/opt
WASI_SDK      = $(WASIX_PREFIX)/wasi-sdk
WASIX_SYSROOT = $(WASIX_PREFIX)/wasix-sysroot/sysroot
WASIX_LIBDIR   = $(WASIX_SYSROOT)/lib/wasm32-wasi
# -pthread/-mthread-model/-ftls-model and the __wasm_init_tls and
# __wasm_signal exports below are what let the runtime snapshot a
# process.  Without them the link fails on __wasm_init_tls (pulled in
# by wasi_thread_start.o); with the flags but not the exports, fork()
# aborts the process at run time instead. 
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

# Directory with /bin/sh and shell utils for the WASIX sandbox, built by
# tests/wasix/sandbox.sh.
WASIX_SANDBOX  = ./wasix-sandbox

# Build the WASIX executable and apply Asyncify instrumentation.
# The shell is enabled; shell commands require a suitable sandbox.
# The build tag prevents reuse of objects from another target.
wasix: wasix-toolchain-check
	$(MAKE) BUILD_TAG=wasix CC="$(WASI_SDK)/bin/clang" \
	CFLAGS="$(CFLAGS) $(WASIX_TARGET)" \
	LDFLAGS="$(WASIX_TARGET) $(WASIX_EXPORTS) $(WASIX_LIBS)" \
	PROGNAME=emil.raw.wasm
	$(WASM_OPT) $(WASIX_ASYNCIFY) emil.raw.wasm -o emil.wasm
	@rm -f emil.raw.wasm

# Verify that the WASIX compiler, sysroot and wasm-opt are available.
# Run tests/wasix/setup.sh to install the pinned toolchain.
.PHONY: wasix-toolchain-check
wasix-toolchain-check:
	@fail=0; \
	[ -x "$(WASI_SDK)/bin/clang" ] || { \
	  echo "Makefile: no clang at $(WASI_SDK)/bin/clang" >&2; fail=1; }; \
	[ -d "$(WASIX_SYSROOT)" ] || { \
	  echo "Makefile: no wasix sysroot at $(WASIX_SYSROOT)" >&2; fail=1; }; \
	command -v $(WASM_OPT) >/dev/null 2>&1 || { \
	  echo "Makefile: no wasm-opt at $(WASM_OPT)" >&2; \
	  echo "  asyncify instrumentation needed for fork() under wasmer" >&2; \
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
	@# Smoke test starts the real binary.
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
# Toolchain: Prebuilt GCC cross-compiler and relibc sysroot.
# Set REDOX_TOOLCHAIN, or run tests/redox_setup.sh to fetch them.
#
# Build constraints:
# - No target-specific CFLAGS are required. relibc provides the necessary POSIX surface
#   (including posix_spawn and pty), making stock C99 flags sufficient.
# - BUILD_TAG is mandatory. Because Redox and native Linux both produce x86-64 ELF objects,
#   omitting it can cause `make` to silently reuse native .o files, resulting in a successful
#   build of the wrong binary.
REDOX_TARGET = x86_64-unknown-redox
REDOX_TOOLCHAIN = $(HOME)/opt/redox-toolchain
REDOX_CC = $(REDOX_TOOLCHAIN)/bin/$(REDOX_TARGET)-gcc
REDOX_NM = $(REDOX_TOOLCHAIN)/bin/$(REDOX_TARGET)-nm

redox: redox-toolchain-check
	$(MAKE) BUILD_TAG=redox CC="$(REDOX_CC)" $(PROGNAME)

# Runs the unit suites inside a real Redox system, via redoxer, which
# boots Redox under QEMU and execs a binary in it.  
REDOXER = redoxer

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

