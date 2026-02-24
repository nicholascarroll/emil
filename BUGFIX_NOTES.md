# Bug Fixes: Undo Corruption & Display Flicker

## Architectural Approach

Rather than adding a flag to suppress undo recording during replay, we split
`editorInsertNewline` into two layers:

- **`editorInsertNewlineRaw(buf)`** — Pure buffer primitive.  Splits the row,
  advances cy, resets cx.  No undo recording.
- **`editorInsertNewline(buf, count)`** — User-facing wrapper.  Calls
  `editorUndoAppendChar` then `editorInsertNewlineRaw`.

(`editorInsertChar` was already a pure primitive — it never recorded undo.)

This gives us three clean categories of caller:

| Caller                        | Which function it calls | Why                                  |
|-------------------------------|------------------------|--------------------------------------|
| User-facing commands          | `editorInsertNewline`  | Needs undo recording                 |
| Undo/redo replay              | `editorInsertNewlineRaw` + `editorInsertChar` | Must NOT record undo  |
| Manual-undo operations (yank) | `editorInsertNewlineRaw` + `editorInsertChar` | Builds its own undo record |

No flags, no special modes, no "secretly suppress recording."  Future code that
needs raw buffer manipulation has a clean, obvious path.

## Files Modified
- `emil.h` — Added `editorInsertNewlineRaw` prototype
- `edit.h` — Added `editorInsertNewlineRaw` declaration
- `edit.c` — Split `editorInsertNewline`, fixed double-recording, forward-order kill data
- `undo.c` — Replay uses raw primitives, forward-order data storage
- `buffer.c` — (unchanged from original — no `in_undo` needed)
- `display.c` — Scroll stabilization for word wrap
- `region.c` — Forward-order undo data, yank uses raw primitives

---

## Bug 1: Undo System Corruption

### Root Cause A: Undo recording during undo/redo replay
`editorDoUndo()` replayed delete-undo records by calling `editorInsertNewline()`,
which internally called `editorUndoAppendChar()`, creating new undo records during
replay and calling `clearRedos()` to destroy the redo chain mid-operation.

**Fix:** `editorDoUndo()` and `editorDoRedo()` now call `editorInsertNewlineRaw()`
and `editorInsertChar()` — neither records undo.

### Root Cause B: Reversed UTF-8 byte storage in delete undo records
`editorUndoDelChar()` stored multi-byte UTF-8 characters with bytes in reversed
order via prepend+reverse.  The old backward replay loop processed bytes
individually, so each byte of a multi-byte character was inserted as a separate
character — producing junk/invalid UTF-8.

`editorUndoBackSpace()` appended bytes in reverse arrival order.

`editorKillRegion()`, `editorKillLine()`, `editorKillLineBackwards()`, and four
transformation sites in `region.c` all stored kill text byte-reversed.

**Fix:** Standardized ALL delete-undo data to **forward order** (text as it
appeared in the buffer from startx,starty to endx,endy):

- `editorUndoDelChar()`: Appends bytes in natural UTF-8 order (no memmove/prepend)
- `editorUndoBackSpace()`: Prepends each byte (backspace delivers right-to-left;
  prepending restores left-to-right order)
- Kill/region operations: `memcpy` instead of reversed loop
- All four reversed-copy sites in `region.c` changed to forward copy

The replay loop in `editorDoUndo()` now iterates **forward**, using
`utf8_nBytes()` to advance by complete UTF-8 characters.

### Root Cause C: Double undo recording in `editorInsertNewlineAndIndent`
Called `editorUndoAppendChar('\n')` then `editorInsertNewline()` which also
called `editorUndoAppendChar('\n')`.

**Fix:** Removed the redundant call.  `editorInsertNewline` (the wrapper)
handles it.

### Root Cause D: Phantom undo records from `editorYank`
`editorYank()` builds its own undo record, but its insertion loop called
`editorInsertNewline()` which created additional unwanted records.

**Fix:** `editorYank()` now calls `editorInsertNewlineRaw()`.

---

## Bug 2: Display Flicker with Word Wrap

### Root Cause A: Inconsistent screen-line computation between scroll and render
`scroll()` computed cursor screen position by calling `countScreenLines()` in a
direct loop.  `setScxScy()` used `getScreenLineForRow()` which reads from the
`screen_line_start` cache.  If the cache was stale between these calls, they
computed different values, causing `rowoff` to oscillate between frames.

**Fix:** Rewrote the word-wrap branch of `scroll()` to use
`getScreenLineForRow()` (the same cache that `setScxScy` uses).

### Root Cause B: Stale cache during render cycle
The screen line cache could be invalidated and lazily rebuilt with different
results mid-render.

**Fix:** Added explicit `buildScreenCache()` calls at the start of
`refreshScreen()` for all visible word-wrap buffers.

### Root Cause C: Unstable backward rowoff walk
The old backward walk from `buf->cy` accumulated line heights imprecisely and
could produce oscillating rowoff values.

**Fix:** New scroll code computes a `target_top` screen line and searches
forward through the cache for the logical row at that position.  Deterministic
and consistent with `drawRows()` rendering.
