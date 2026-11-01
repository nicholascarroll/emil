# refcheck

Compares a new commit of emil with an old one and reports only what the
new commit changed.  It is for refactoring: the default assumption is that
behaviour is unchanged, and every check looks for evidence that it did
change, or that something else got worse.

Every check is deterministic.  No model is involved yet.

```sh
python3 refcheck/run.py --old main --new HEAD          # report in refcheck-work/
python3 refcheck/replay.py --seed 3 --keep 339,340 OLD NEW   # replay a witness
```

In CI, `.github/workflows/refcheck.yml` runs it on every push (old = the
commit the push replaced) and puts the report in the job summary.  It
never fails the run.

## Checks

| Check | Compares | Reports |
|---|---|---|
| `warnings` | `HAL_WARNINGS` diagnostics, default and minimal (`EMIL_DISABLE_SHELL`) configs | warnings and errors not present in the old commit |
| `tests` | `make test` suites and source invariants | suites that now fail (rerun once; a non-repeating failure is reported as flaky) |
| `newtests` | the new commit's changed tests, run against the old code | tests that fail on old and pass on new: they see a behaviour change |
| `stale` | functions defined in old but not new | remaining mentions in code, comments, man page and docs |
| `footprint` | each changed function's transitive reads and writes of global state, from clang's AST | state a changed function now reads or writes, or no longer does, with where each comes from and how many unchanged callers inherit it |
| `difffuzz` | old and new driven by identical seeded keystrokes | divergences in buffers, cursor, mark, echo area, kill ring, registers, windows, files on disk; new sanitizer errors and hangs |
| `coverage` | test-suite coverage of changed lines | changed lines no test runs, and whether the fuzzer runs them |

The report shows findings and incomplete checks only.  A check that could
not do its job is listed under *Incomplete*, never as clean: a build that
did not run, seeds that were nondeterministic, a flaky suite, changed
lines the fuzzer never reached (its silence says nothing about them).

## Change context

After the findings, the report has a collapsed *Change context* section:
for every function the commit changed, added or removed, a shortest caller
chain from a top-level entry (`main -> processKeypress -> dispatchBuffer ->
findFile`), its direct callers and callees, the global state it reads and
writes itself (with the size of its footprint including callees), and how
many of its lines the tests and the fuzzer run.  It is background, not
findings, and comes from data the checks already computed; a part whose
check was skipped says "not measured".

## Differential fuzzing

`difffuzz.c` is `tests/fuzz_keys.c` with a `trace` mode added.  The same
driver source is compiled against each commit (`build.sh`, under
ASan+UBSan), and after every key it prints a hash of each part of the
editor state.  Per seed, `difffuzz.py` runs old and new; if they differ it
runs old again (old disagreeing with itself means the harness is
nondeterministic, so the seed is set aside) and new again (new
disagreeing with itself is its own finding).

A divergence is shrunk by deleting keys while the same state fields still
differ (the driver reports how many keys it skipped, and a mismatch stops
the shrink rather than producing a false witness), then located by running
the shrunk witness on a coverage build and recording which changed lines it
executes.  Divergences whose changed lines are equal, or one a subset of
the other, are reported as one finding, whatever downstream state they
reach.

### Footprints

`footprint.py` reads clang's JSON AST of every file and records, per
function, the global state it reads and may write: file-scope and
function-static variables and paths into them up to two members deep
(`E.statusmsg`, `E.buf->dirty`).  Taking an address or passing an array
on counts as both read and write.  Accesses through a pointer parameter
are kept as `$0->dirty` and rewritten at each call in terms of the
argument, so `markBufferDirty(E.buf)` gives `E.buf->dirty`, and replacing
a global access by a parameter the caller passes the same global to is
not a change.  Accesses through a local pointer, or a parameter given
something local, become the pointed-to type: `(buffer)->dirty`, some
buffer's dirty flag.  Footprints are transitive over the call graph.

The check reports a changed function's footprint difference once, at the
function, leaving out what a changed callee already explains, and says
how many unchanged callers inherit it.  It works on sets, so it sees a
function gain or lose an access, not lose one of several ways of making
it: dropping one of two calls that both write some buffer's `dirty` is
invisible here, and only the behaviour check can see it.

### The screen

emil's terminal output goes into libvterm (`libvterm-dev`), and what is
compared is the emulator's grid: every cell's characters, width,
attributes and colours, plus the cursor's position and visibility.  The
bytes themselves are not compared, so drawing the same screen a different
way is not a divergence.  Escape sequences libvterm does not recognise are
hashed too, so they cannot hide a difference.

A snapshot is taken whenever the editor asks for a key (when it has
finished drawing), inside prompts, isearch and the other modal loops as
well as at the top level, and a step's `screen` field covers every
snapshot in it.  The report shows the first snapshot that differs, the
rows that differ, and a marks line under each row for attributes (`R`
reverse, `B` bold, `U` underline, `C` colour, `*` several).

### Files and the sandbox

Each run gets a fresh sandbox (`sandbox.sh`): the shared fixtures plus a
line-ending corpus (`*.le`: LF, CRLF, lone CR, CR CR LF, no final newline,
CRLF without one, mixed, empty, NUL, invalid UTF-8) and a directory.  A
file-name token in the key generator types one of those names, or a
directory, a trailing-slash path, a missing directory or a new file, into
`C-x C-f`, `C-x i` or `C-x C-w`.  With 300 seeds this runs 89 of 128
executable lines of `editorOpenBody` and 67 of 76 of
`insertFileAtPathBody`; what is left is size limits, read errors,
read-only files and other processes' locks.

While the editor runs, the sandbox is the whole filesystem: the driver
wraps `open`, `fopen`, `unlink`, `mkstemp`, `opendir`, `stat`, `access`
and `chdir` so writes outside it fail with `EACCES`, reads with `ENOENT`,
and `chdir` out of it fails.  Without this, a typed absolute path writes
into the real filesystem (as root: anywhere), and reads of `/tmp` differ
from run to run.

### Determinism

Time is a function of the key index (100 ms per key) and `setitimer` is a
no-op, so the editor's 2-second file check and 50 ms `stat` deadline fire
at the same keys in every run.  Fixtures are dated 2001, so a write during
the run always changes a file's mtime (emil compares whole seconds).  With
these, 2000 seeds of `main` against itself under 3 parallel workers are all
clean.

## Text and locale

All text refcheck reads from tools and files, and all it writes, goes
through `textio.py`: always UTF-8 whatever the machine's locale, and never
fatal on a byte that is not valid UTF-8.  Text for people shows such a byte
as `\xff` (emil's tests and fixtures contain them on purpose); text used as
data (paths, clang's AST) keeps it byte for byte.  Every tool runs under
`LC_ALL=C.UTF-8`, because tool messages depend on the locale (gcc quotes
`‘x’` in a UTF-8 locale and `'x'` in C) and refcheck compares them across
commits and machines; the warning comparison also treats both quote styles
alike.  Under an ASCII-only locale, refcheck produces the same report as
under UTF-8.

## Not done yet

- The terminal's own tab stops are libvterm's defaults (every 8
  columns), which is what a real terminal starts with.
- File-loading paths that depend on the machine rather than the input
  (permissions, which differ as root; locks held by other processes) are
  not exercised.
- Clone search on deleted bodies, CodeQL, CBMC,
  instruction counts.
- The model router (reading comments and the commit message for stated
  exceptions and invariants).
- A push is compared as one range, not commit by commit.
- A witness's location lists every changed hunk it ran, including ones
  it only passed through on the way to the divergence.
