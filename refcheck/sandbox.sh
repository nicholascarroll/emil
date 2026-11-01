#!/bin/sh
# Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT
# sandbox.sh DIR: (re)create the fixture directory a difffuzz sequence
# runs in.  Same files as tests/fuzz_keys.sh, so the file, completion and
# ctags commands have something to find.  Every run of every build gets
# a fresh copy at the same path, so file state is comparable.
#
# The line-ending corpus (*.le) exists for the file-loading paths
# (C-x C-f, C-x i, save): every combination of CRLF, lone CR, missing
# final newline, empty file and NUL byte that the loaders treat
# differently.  difffuzz.c's FILE_NAMES must list the same names.
set -eu
DIR=$1
rm -rf "$DIR"
mkdir -p "$DIR/sub"
cd "$DIR"
printf 'int foo(int x);\n' > a.h
printf '#include "a.h"\nint foo(int x) { return x + 1; }\nint main(void) { return foo(1); }\n' > a.c
printf 'Some notes.\nA second line with \303\251t\303\251.\n' > notes.txt
printf 'one\n' > sub/one.txt
printf 'two\n' > sub/two.txt
printf 'foo\ta.c\t/^int foo(int x) {$/;"\tf\nfoo\ta.h\t1;"\tp\nmain\ta.c\t3;"\tf\nalpha\tnotes.txt\t1\n' > tags
# Line-ending corpus.  \015 is CR, \000 is NUL.
printf 'one\ntwo\n'               > lf.le
printf 'one\015\ntwo\015\n'       > crlf.le
printf 'one\ntwo'                 > nonl.le
printf 'one\015\ntwo'             > crlfnonl.le
printf 'abc\015'                  > cr.le
printf 'abc\015\015\n'             > crcr.le
printf 'a\015b\nc\n'              > midcr.le
printf 'x\ny\015'                 > tailcr.le
printf 'one\015\ntwo\n'           > mixed.le
printf ''                         > empty.le
printf '\n'                       > nl.le
printf 'a\000b\n'                 > nul.le
printf '\015\n\015\n'              > blank.le
printf 'ok\n\377bad\n'            > badutf8.le
# Every fixture gets the same old timestamp.  emil notices a file
# changed on disk by its mtime in whole seconds and its size; with real
# creation times, whether a write during the run changes the mtime
# depends on which side of a second boundary it lands.  From 2001, any
# write during the run is a change.
find . -exec touch -h -d '2001-01-01 00:00:00' {} +
