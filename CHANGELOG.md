## [Unreleased]
- Fixed forward incremental search (`C-s`) starting from the top of the buffer
  instead of from point. The row-stepping loop was seeded with -1 for a forward
  search and only with the cursor's row for a backward one. Search now runs from
  where it was started, accepts a match beginning at point, and wraps to the top
  only after passing the end of the buffer. Editing the pattern re-searches from
  the original starting point rather than from the previous match, so deleting a
  character no longer strands the search further down the buffer.
- Fixed a selection in one window being drawn or hidden according to whether the
  buffer in *another* window had a valid mark. `markInvalidBuf()` takes the
  buffer to test; `markInvalidSilent()` remains as the wrapper for the focused
  buffer.
- Fixed plain Shift-Tab doing nothing. `unindent` looped on the raw prefix
  argument, which is 0 when no prefix is given; it now applies `UARG_COUNT` like
  every other command in `edit.c`.
- Fixed Down in a prompt destroying typed text. Down with no history browsing in
  progress is now a no-op, and stepping off the end of history restores the text
  that was being typed, as Emacs does.
- Fixed `absolutePath` producing `//name` when the working directory is `/`,
  which made the same file compare unequal to itself and open in two separate
  buffers, each with its own undo stack and lock state.
- Fixed `C-x =` reporting a screen row computed from the first window's scroll
  offset rather than the focused window's.
- Fixed a multi-byte character being dropped when a signal (SIGWINCH from a
  terminal resize, SIGCONT from foregrounding) arrived between its lead byte and
  a continuation byte. The UTF-8 assembly reads now retry on EINTR, as the
  escape-sequence reader already did.
- Fixed the status bar truncating a long filename mid-character, emitting
  invalid UTF-8 to the terminal, and returning an untruncated length that made
  the caller read past the end of its buffer on very wide terminals.
- Fixed nested prompts corrupting the editor buffer. `editorPrompt` saved the
  current buffer into `E.edbuf` without restoring the outer prompt's value, so
  opening a second prompt from inside the first (`C-x C-f`, `M-x`, `C-x b`,
  `C-x i`, `C-x C-w`, `M-|`) left `E.buf` pointing at `*minibuffer*` once the
  outer prompt exited. Every subsequent keystroke then edited the minibuffer
  while the windows still showed the real file.
- Fixed a crash in `revert-buffer` on a buffer with no filename, reachable by
  starting `emil` with no arguments.
- `revert-buffer` now refuses when the file no longer exists, rather than
  silently replacing the buffer with an empty one. The old behaviour destroyed
  the undo stack along with the buffer, so the work could not be recovered, and
  left a clean buffer that let `C-x C-c` exit without warning.
- Fixed `zap-to-char` corrupting the buffer when given a non-character key.
  Navigation and Meta keys arrive as tokens above 255 and were truncated into
  the UTF-8 lead-byte range, so the kill could cut a multi-byte character in
  half and leave the buffer unsavable. Such keys are now rejected.
- Fixed reverse incremental search (`C-r`) moving forward past point when a
  match existed on the cursor's own row, and landing on the first rather than
  the last match when stepping back to an earlier row.
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

