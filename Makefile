# Remember to keep the version number up to date
VERSION = 0.1.0

PROGNAME = emil
PREFIX = /usr/local
SHELL = /bin/sh

# Documentation defaults
MAN_SOURCE = emil.1
MAN_SUBDIR = .

# Standard C99 compiler settings
CC = cc

# Enable BSD and POSIX features portably
DEFAULT_CFLAGS = -std=c99 -Wall -Wextra -Wpedantic -Wno-pointer-sign -D_DEFAULT_SOURCE -D_BSD_SOURCE -O2

# The "User" overrides (blank by default)
CFLAGS = 
LDFLAGS = 

# The actual flags used for compilation
ALL_CFLAGS = $(DEFAULT_CFLAGS) $(CFLAGS)

# Installation directories
BINDIR = $(PREFIX)/bin
MAN_BASEDIR = $(PREFIX)/man
DOCDIR = $(PREFIX)/share/doc/emil

# Source files
OBJECTS = main.o wcwidth.o unicode.o buffer.o region.o undo.o transform.o \
          find.o pipe.o register.o fileio.o terminal.o display.o message.o \
          keymap.o edit.o prompt.o util.o completion.o history.o base64.o

# Default target
all: $(PROGNAME)

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
	-mkdir -p $(BINDIR) 
	-mkdir -p $(MAN_BASEDIR)/$(MAN_SUBDIR)/man1
	-mkdir -p $(DOCDIR)
	cp $(PROGNAME) $(BINDIR)/
	-cp $(MAN_SOURCE) $(MAN_BASEDIR)/$(MAN_SUBDIR)/man1/$(PROGNAME).1 2>/dev/null
	-cp README*.md $(DOCDIR)/ 2>/dev/null
	chmod 755 $(BINDIR)/$(PROGNAME)

uninstall:
	# Remove the binary
	rm -f $(BINDIR)/$(PROGNAME)
	
	# Remove the specific man page installed
	# This uses the current MAN_SUBDIR to find the right file
	rm -f $(MAN_BASEDIR)/$(MAN_SUBDIR)/man1/$(PROGNAME).1
	
	# Clean up documentation
	rm -f $(DOCDIR)/README*.md
	
	# Attempt to remove directories only if they are empty
	# We use '-' to ignore errors if other files exist in these dirs
	-rmdir $(DOCDIR) 2>/dev/null
	-rmdir $(MAN_BASEDIR)/$(MAN_SUBDIR)/man1 2>/dev/null
	-rmdir $(MAN_BASEDIR)/$(MAN_SUBDIR) 2>/dev/null

# Cleanup
clean:
	rm -f $(OBJECTS) $(PROGNAME)

# Testing
test: $(PROGNAME)
	./tests/run_tests.sh

check: test

# Sorry Dave
hal:
	$(MAKE) format
	$(MAKE) clean
	for f in *.c; do clang-tidy $$f -- -I. ; done
	$(MAKE) CFLAGS="$(CFLAGS) -D_POSIX_C_SOURCE=200112L -Werror" $(PROGNAME)
	$(MAKE) test

# Development targets
debug:
	@GIT_VERSION="`git describe --tags --always --dirty 2>/dev/null || echo $(VERSION)`"; \
	$(MAKE) VERSION="$$GIT_VERSION" CFLAGS="$(CFLAGS) -g -O0" $(PROGNAME)

format:
	clang-format -i *.c *.h

# Platform-specific variants (POSIX Compatible)
android:
	$(MAKE) CC=clang \
	CFLAGS="$(CFLAGS) -fPIC -fPIE -DEMIL_DISABLE_PIPE" \
	LDFLAGS="-pie" \
	$(PROGNAME)

msys2:
	$(MAKE) CFLAGS="$(CFLAGS) -D_GNU_SOURCE" $(PROGNAME)

minimal:
	$(MAKE) CFLAGS="$(CFLAGS) -DEMIL_DISABLE_PIPE -Os" $(PROGNAME)

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

.PHONY: all install uninstall clean test check hal debug format android msys2 minimal solaris darwin help
