## [Unreleased]
- Fixed a window's top line going blank after an edit deleted the row
  it was scrolled into. `adjustAllPoints` moved `rowoff` to the
  deletion's first row but carried `skip_sublines` across unchanged, so
  it could name a sub-line the surviving row does not have; `drawRows`
  then skipped past the row's last sub-line and emitted nothing.
  `scroll()` repaired this for the focused window, so it showed only in
  a window that was not focused. Introduced with the `rowoff`
  adjustment itself.
- Fixed the same blank top line arising without a deletion: a top row
  that shrank, or a terminal that widened, could leave a stored
  `skip_sublines` past the row's last sub-line. `topSet()` deliberately
  does not clamp on write (that would be a whole-row walk per write),
  so `drawRows` now clamps on the read and draws the row's last
  sub-line. Pre-existing.
- Fixed the terminal-ownership pty scenarios silently skipping instead
  of failing when the editor genuinely fails to enter raw mode. The
  probe for "can this platform report the slave's termios through the
  master" asked whether ECHO was off while the editor ran -- which is
  also the defect the scenarios exist to catch, so a real failure was
  indistinguishable from an unobservant platform, and the probe won.
  It now opens its own pty pair and checks that a flag set on the slave
  is reflected through the master in both directions, with no editor
  involved. Confirmed by mutation: with ECHO and ISIG left on, three
  scenarios previously skipped on Linux and now fail.
- Corrected the WASIX skip rationale, which reasoned about `wasi-libc`
  while the target builds against `wasix-libc`. Only the `warnings`
  skip is a platform gap (no `F_GETLK`/`F_SETLK`); `subprocess` and
  `shell` are skipped because this target chooses `-DEMIL_DISABLE_SHELL`,
  and `writeall` fails to link for threading-model reasons rather than
  for the absence of `fork` or of signals, both of which WASIX has.
  No behaviour change; the same four suites are skipped. README
  updated to match.
- Corrected two comments that overstated the viewport rewrite.
  Viewport questions are bounded by the window height in screen lines,
  not in bytes: `screenWalkStart()` costs the bytes before its
  sub-line, and `linesBack()` costs a whole row to learn its last
  sub-line index. A frame is independent of `numrows`, which is what
  #111 achieved, but still scales with the cursor's depth inside a
  single long wrapped row. Measured on one 4 MB row: 0.06 ms at the
  start, 167 ms at 3.2 MB in. Page-down on the same row improved from
  1024 ms to 201 ms, and page-up on short rows regressed from 0.036 ms
  to 0.072 ms, which the original notes recorded as a wash.
- Fixed the hard-link tests failing on Haiku, where `link()` is not
  available. They asserted that the call succeeds, which is a claim about
  the filesystem rather than about emil -- and a filesystem without hard
  links cannot have the bug they guard. They now skip.
- Fixed the terminal-ownership pty scenarios failing on MSYS2. They
  assumed the raised `SIGTSTP` is always discarded, which POSIX requires
  only for an orphaned process group: Linux and illumos discard it,
  Cygwin stops the process anyway. A stopped editor is supposed to have
  handed the terminal back, so the assertion had it exactly backwards.
  The scenarios now distinguish stopped from running, and where the stop
  takes effect they send `SIGCONT` and assert the editor reclaims the
  terminal -- the same repair, reached the ordinary way.
- Tests can now skip themselves. `TEST_SKIP` reports a platform that
  cannot host a test rather than passing silently, and both the unit
  suites and the pty scenarios count and print their skips, so coverage
  cannot quietly shrink where nobody is looking.
- Fixed the terminal-ownership pty scenarios failing on illumos. They
  asserted that `tcgetattr()` on the pty master reports the slave's
  termios, which is a Linux/BSD convenience: an illumos pty is a STREAMS
  device whose master has no terminal semantics of its own, so the call
  fails and the suite reported "editor did not start in raw mode" against
  an editor that had. The scenarios now skip where the state cannot be
  observed, and the Ctrl-C-after-suspend check -- which needs no termios
  visibility and passed on OpenIndiana throughout -- now covers all three
  suspend paths, so those platforms keep real coverage.
- Saving now direct-writes hard-linked files.
- Saving no longer replaces a FIFO, socket or device node.
- A file whose directory is not writable can now be saved.
- #107 Removed the direct-overwrite prompt on ENOSPC.
- Higlighting of matches in `query-replace` #106.
- On file save, a confirmation prompt if file on disk is newer
- Fixed the editor being left with the terminal in cooked mode after
  `C-z`, `C-x z` or `C-x C-z` when the raised `SIGTSTP` is discarded.
- Fixed a heap-buffer-overflow in `clampCursorToViewport`.
- `fuzz_undo` now runs a set of seeds rather than the pinned seed 1.
- The shared test harness now gives its window a realistic height.

## [0.9.3]
- query-replace better error handling
- Fixed #103: After failing search, cursor position wrong
- Fixed heap corruption when undoing inside a prompt. 
- Fixed trailing blank lines being undeletable from the end of a buffer.
- A lone CR is no longer reported as a DOS line ending. A classic-Mac
  CR-only file keeps its CRs on save, so announcing a conversion to Unix
  endings promised something that does not happen.
- Opening a file that lacks a final newline, or that uses DOS line endings,
  now says so in the status line and states what will happen on save. The
  buffer stays clean, so an unedited file is never rewritten.
- Fixed piped input without a final newline leaving the last row non-empty,
  breaking the buffer invariant that every mutation path relies on.
- A file's trailing newline is now a buffer invariant.
- `C-x C-s` on a buffer with no unsaved changes now reports
  "(No changes need to be saved)" instead of rewriting the file.
- Fixed a heap-use-after-free in `scrollViewport`: `rowoff` was not
  clamped on entry, so scrolling up in word-wrap mode after an edit had
  deleted rows read a freed row. Pre-existing; found by `fuzz_undo.c`.
- Radical refactor of undo; implemented issues #104,#105. This fixes
  #102: undo of newlines at end of buffer fails
- A run of typing or deleting now becomes a new undo step every 20
- Fixed failure to undo insertions on the last line of the file #102.
- Fixed incremental search (`C-s`) starting from the top of the buffer #101
- Fixed a selection being drawn or hidden according to whether *another* window
  had a valid mark. 
- Fixed plain Shift-Tab doing nothing.
- Fixed Down in a prompt destroying typed text.
- Fixed `absolutePath` producing `//name` when the working directory is `/`.
- Fixed `C-x =` reporting a screen row computed from the wrong window.
- Fixed a multi-byte character being dropped on terminal resize.
- Fixed the status bar truncating a long filename mid-character.
- Fixed nested prompts corrupting the editor buffer. 
- Fixed a crash in `revert-buffer` on a buffer with no filename, reachable by
  starting `emil` with no arguments.
- `revert-buffer` now refuses when the file no longer exists.
- Fixed `zap-to-char` corrupting the buffer when given a non-character key.
- Fixed reverse incremental search (`C-r`) moving forward past point.
- Removed the between-rows interrupt poll from interactive search. 
- Rules for punctuation at right edge of screen in word wrap mode
- Cope with background/foregrounding and terminal resize while in the minibuffer
- Unrecognised command-line options now report to stderr and exit nonzero
- man page updates.
- The man page installs to `$(PREFIX)/share/man` rather than `$(PREFIX)/man`,
  and LICENSE installs to `$(PREFIX)/share/licenses/emil`. Upgrading from
  0.8.0 leaves an orphaned `/usr/local/man/man1/emil.1` that `make uninstall`
  will not remove; delete it by hand.
- `make hal` builds with an expanded warning set as errors, and runs the
  test suite under `-D_FORTIFY_SOURCE=3` so the fortified build is actually
  executed rather than only compiled.
- In atomic write, preserve owner. #98.
- Clamp universal argument at 1 million #99.
- Allow entry of newline with `C-q C-j` in `replace-regexp` #96.
- Fixed bug in rectangle edit that caused UTF-8 corruption.

### Correction to 0.7.0
- #90 (read-only buffer corruption from kill-rectangle) was listed under Known
  Bugs in 0.7.0 and fixed during 0.8.0 by the `rejectIfReadOnly` guard at the
  rectangle command entry points, but was never recorded as closed. It is
  fixed.

## [0.8.0] - 2026-07-28
- replace-regexp now more conformant to Emacs behaviour
- Rewrote the escape-sequence decoder as an explicit state machine.
- Fixed ESC-arrow up "[A") into the buffer as text #95.
- Fixed getCursorPostion hanging. #94
- Deactivate mark after region to register and backspace selection #93
- Fixed F12 (and PuTTY F4) discarding unsaved work via an undocumented panic.
- Fixed SS3 escape sequences inserting text.
- Fixed minibuffer sizing, wrapping, and prompt cursor position for multi-byte text.
- Removed probe that tries to get the screen size if kernel doesn't know
- Hardcoded English for messages and man pages and removed Spanish and Chinese

## [0.7.0] - 2026-07-20
- Fixed crash yanking a rectangle into an empty buffer.
- Fixed crash scrolling an empty buffer in word-wrap mode.
- Fixed out-of-bounds read in backspace over stray UTF-8 continuation bytes.
- Fixed duplicate scratch buffers when killing the last buffer shown in two windows.
- Refactored C-y rectangle handling.
- Reverted to a single README (in English with Chinese build instructions merged in)
- Corrected punctuation in Chinese man page
- Fixes for pipe shell
- Fixes for macros
- Fixed undo bug affecting read-only buffers

### Known Bugs
- #90 Read-only buffer corruption from kill-rectangle

## [0.6.0] - 2026-07-18
- Thai/Lao/Khmer boundaries and Chinese line-breaking: word wrap 
  now follows 行首禁则  #87, #88.
- Shell commands (M-|, M-!) can now be cancelled.
- Opening a directory now fails with an error, #67
- Kill-ring save/restore now preserves rectangle metadata.
- Removed the bundled wcwidth table (widechar_width_c.h)
- More effective cancel (C-g) during interactive search

## [0.5.0] - 2026-05-08
- CJK and Indic sentence movement: #69, #71, #72, #73, #74
- Replaced the Memory Budget feature with simpler File size limit (1GB)
- Removed undo limit
- Palette for emojis and symbol chars #84.
- Now uses system wcwidth
- Temporarily added option to debug build `M-x toggle-wcwidth` to use bundled wcwidth
- Fixed #46, #54, #65, #66.

## [0.4.0] - 2026-04-23
- Remove dependency on subprocess.h
- Removed the dict shell script
- Refactored to use a mutation layer
- Implemented warning message system in RHS of status bar
- Remapped C-h to help message
- Resolved issues #29, #30, #31, #33, #40, #41, #49, #56, #57, #58
- Implemented Find File Read Only
- Open read-only if locked
- Changed memory budget to a simplified model
- Renamed EMIL_MAX_OPEN_BYTES to EMIL_BYTES_BUDGET

## [0.3.0] - 2026-03-30
Architecture and robustness improvements. Unstable.
- Unified memory budget (EMIL_MAX_TOTAL_BYTES, default 1 GB). View budget with
  `M-x editor-status`. Set budget at build time via -D flag.
- 2038 date cutover safe.
- Better signal  handling
- Mitigation of heap fragmention - compaction on save.
- Refactored to fully embrace global state and tidied naming conventions.
 
## [0.2.1] - 2026-03-19
Initial prerelease, The editor is unstable and not reliable for production use.
- Feature complete.
- Documentation complete.

