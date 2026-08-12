# Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
# SPDX-License-Identifier: MIT

# Remember to keep the version number up to date
VERSION = 0.9.3

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
	rm -f $(OBJECTS) $(PROGNAME)

# Testing
test: $(PROGNAME)
	@echo "Makefile: Launching tests with CC=$(CC)"
	@uname -a
	CC="$(CC)" CFLAGS="$(ALL_CFLAGS)" LDFLAGS="$(LDFLAGS)" ./tests/run_tests.sh

check: test

# EMIL_DEBUG_ROW_CACHE recomputes and compares cached_width on every
# cache hit (see calculateLineWidth in wrap.c).  It belongs here rather
# than in a default build because the check is the same whole-row walk
# the cache exists to avoid -- on a 50 MB line it costs 439 ms per
# frame -- and here because this is the build the pre-merge run uses,
# so the §4.10 invalidation protocol is exercised by every suite and by
# the fuzzer before anything is proposed for merge.
sanitize:
	$(MAKE) clean
	$(MAKE) CFLAGS="-g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -DEMIL_DEBUG_ROW_CACHE" \
	        LDFLAGS="-fsanitize=address,undefined" test

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
	@echo "  check     Alias for test"
	@echo "  format    Format code with clang-format"
	@echo "  hal       HAL-9000 compliance"

.PHONY: all install uninstall clean test check sanitize hal debug format android msys2 minimal solaris darwin help

# Terminal-level integration tests: drives the real binary under a
# pseudo-terminal (also run at the end of `make test`).
test-pty: $(PROGNAME)
	$(CC) $(ALL_CFLAGS) -o tests/decoder_pty_test tests/decoder_pty_test.c
	./tests/decoder_pty_test ./$(PROGNAME)
	rm -f tests/decoder_pty_test

