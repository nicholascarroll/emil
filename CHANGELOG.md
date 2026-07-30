## [Unreleased]
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

