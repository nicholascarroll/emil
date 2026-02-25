
# emil — High-Level Design

## 1. Purpose and Scope

This document describes the internal architecture of emil, a small terminal
text editor for UTF-8 files on VT100-compatible terminals. It covers the
system model, buffer management, rendering, scrolling, undo/redo, and
multi-window coordination.

The design prioritises clarity and correctness over performance. Optimisations
are identified but deferred to a later phase, after the core architecture is
proven stable.

### 1.1 Assumptions

- **Array of lines.** The buffer is an array of logical lines (`erow`). Each
  line stores its raw UTF-8 bytes in a flat `chars` array. This is simple,
  cache-friendly for typical text, and easy to reason about.
- **UTF-8 only.** All buffers contain valid UTF-8. Files that fail validation
  are rejected at load time. Internal operations preserve UTF-8 validity as an
  invariant.
- **Word wrap is display-only.** Soft wrapping never modifies the buffer. A
  logical line may span multiple screen lines (called “sub-lines”), but the
  buffer always stores the original unwrapped text.
- **Feature-complete.** No new functional features will be added. Work focuses
  on architecture, correctness, and maintainability.
- **Broad terminal support.** The editor targets VT100-compatible terminals.
  The rendering system uses only cursor positioning (CSI H), erase-to-end-of-
  line (CSI K), reverse video (CSI 7m / CSI 0m), and clear-below (CSI J).
  Scroll region manipulation and line insert/delete are not used by the core
  renderer; they may be added later behind a capability flag.

### 1.2 Constraints

- C99, POSIX.1-2001. No external dependencies.
- Single-threaded, synchronous I/O.
- No background processes except explicit shell integration.

## 2. System Model

These invariants govern the relationship between buffers, windows, and the
screen. They underpin both correctness reasoning and the optimisation
strategy in §7, and are enforced throughout the implementation.

### 2.1 Single Source of Mutation

Editing operations act only on the buffer associated with the focused window.
At any instant:

- Exactly one buffer may be mutated.
- Mutations are synchronous and single-threaded.
- All rendering is read-only with respect to buffer state.

Multiple buffers may coexist, but only one is active for modification at a
time. Focus must move to a window displaying a different buffer before that
buffer can be edited.

### 2.2 Windows as Passive Views

Windows do not own text. They are viewports onto buffers:

- Multiple windows may display the same buffer.
- A window showing a different buffer is entirely unaffected by edits
  elsewhere.
- A window showing the same buffer reflects edits only insofar as its visible
  region or metadata (e.g. line count, dirty flag) are affected.
- Non-focused windows do not independently initiate scroll, cursor movement,
  or wrap changes.

Transient displays such as completion lists are not special UI layers. They
are ordinary buffers shown in ordinary windows, marked with `special_buffer`
for lifecycle management only. Showing or dismissing a completion list is
equivalent to creating or destroying a window and recomputing the vertical
layout. There are no overlays, floating panels, or composited layers.

### 2.3 Full-Width Row-Stack Layout

All windows span the full terminal width. The screen is partitioned solely
along the vertical axis into contiguous ranges of rows:

```
┌─────────────────────────────────┐
│  Window 0 content               │  rows 1 .. h₀
│  (height h₀)                    │
├─────────────────────────────────┤
│  Status bar 0                   │  row h₀+1
├─────────────────────────────────┤
│  Window 1 content               │  rows h₀+2 .. h₀+1+h₁
│  (height h₁)                    │
├─────────────────────────────────┤
│  Status bar 1                   │  row h₀+h₁+2
├─────────────────────────────────┤
│  Minibuffer                     │  last row
└─────────────────────────────────┘
```

There are no vertical splits, left/right borders, gutters, or margins. Every
element occupies columns `0 .. screencols−1` for its row range.

This property has important consequences:

- Terminal operations can be expressed purely in terms of row ranges.
- No column clipping is required.
- Scroll regions (§7.2) map directly to windows.
- The rendering logic in `drawRows` never needs to consider horizontal
  window boundaries.

### 2.4 Layout Changes vs. Content Changes

Two qualitatively different kinds of updates exist:

**Content changes** affect only windows displaying the mutated buffer. Windows
on other buffers require no work at all.

**Layout changes** — creation, destruction, or resizing of windows — alter the
mapping from windows to screen rows and require a full redraw. These are
infrequent (window split, completion popup, terminal resize).

This distinction is central to the optimisation strategy in §7.1.

## 3. Buffer Architecture

### 3.1 The `erow` Structure

Each logical line is represented by an `erow`:

```c
typedef struct erow {
    int size;           /* byte count of chars (excluding NUL) */
    uint8_t *chars;     /* raw UTF-8 content, NUL-terminated */
    int cached_width;   /* display width in columns, or -1 if stale */
} erow;
```

**Stale width sentinel:** `cached_width` uses `-1` to indicate a stale
value. This is necessary because an empty line has a legitimate display
width of `0` columns — using zero as a sentinel would conflate “stale”
with “empty line.”

**What was removed and why:**

- `render` / `rsize` / `renderwidth` / `render_valid` — The render buffer was
  a pre-expanded copy of `chars` with tabs converted to spaces and control
  characters decorated. It is no longer needed because
  `renderLineWithHighlighting` reads directly from `chars` and expands
  tabs and control characters on the fly into the output `abuf`.
- `width_valid` — Replaced by the `cached_width == -1` sentinel. A single
  field serves both purposes, reducing `erow` to three fields.

**Single source of truth:** `chars` is the only place text exists. No secondary
buffers are maintained for display purposes.

### 3.2 The `editorBuffer` Structure

A buffer holds the file content and editing state:

```c
struct editorBuffer {
    int cx, cy;             /* cursor position (byte offset, row index) */
    int markx, marky;       /* mark position for region operations */
    int numrows;            /* number of logical lines */
    int rowcap;             /* allocated capacity of row array */
    erow *row;              /* array of lines */
    int dirty;              /* modified since last save */
    int word_wrap;          /* soft wrap enabled */

    /* Undo */
    struct editorUndo *undo;
    struct editorUndo *redo;
    int undo_count;

    /* Screen line cache (see §5.2) */
    int *screen_line_start;
    int screen_line_cache_size;
    int screen_line_cache_valid;

    /* ... other fields (filename, mark, completion, etc.) */
};
```

The buffer owns the canonical cursor position `(cx, cy)`. See §6.1 for
how this interacts with multi-window cursor snapshots.

### 3.3 The `editorWindow` Structure

A window is a viewport onto a buffer:

```c
struct editorWindow {
    struct editorBuffer *buf;
    int focused;
    int scx, scy;       /* screen cursor position within window */
    int cx, cy;          /* saved buffer cursor (updated on window switch) */
    int rowoff;          /* first visible logical line */
    int coloff;          /* horizontal scroll offset (non-wrap mode) */
    int height;          /* window height in screen lines */
};
```

Multiple windows may display the same buffer. Each window maintains its own
`rowoff` and `coloff` independently, consistent with the passive-view model
(§2.2).

## 4. Rendering

### 4.1 Philosophy

Rendering is **stateless and single-pass**. On each frame, the renderer reads
raw bytes from `chars` and emits terminal-ready sequences directly into a
disposable `abuf`. No intermediate render buffers exist. The `abuf` is written
to the terminal in a single `write()` call.

This trades CPU cycles (re-expanding tabs and control characters each frame)
for architectural simplicity: there is no render state to invalidate, no
cache coherency to maintain between edits and display, and no class of bugs
where the render buffer is out of sync with `chars`.

Because rendering is read-only with respect to buffer state (§2.1), no
locking or ordering constraints exist between rendering different windows.

### 4.2 The Rendering Pipeline

Each frame executes:

```
refreshScreen()
  ├── clampAllWindows()  — bounds-check rowoff for every window
  ├── buildScreenCache() for each visible buffer (if stale)
  ├── scroll()           — compute rowoff, coloff, skip_sublines (focused only)
  ├── for each window (top to bottom):
  │     ├── drawRows()     — emit visible content into abuf
  │     └── drawStatusBar()
  ├── drawMinibuffer()
  ├── position cursor
  └── write(abuf) → terminal
```

**`refreshScreen`** hides the cursor, homes it, renders all windows and
chrome, positions the cursor for the focused window, shows the cursor, and
flushes the `abuf` in one write.

Non-focused windows do not run `scroll()` — their `rowoff` is fixed at
whatever it was when they lost focus (§2.2). Only `clampAllWindows` ensures
their `rowoff` remains within bounds.

**`drawRows`** walks logical lines from `rowoff` forward. For each line:

- **Without word wrap:** Render the portion from `coloff` to
  `coloff + screencols`, expanding tabs and control characters on the fly.
  Emit CSI K to clear trailing content.
- **With word wrap:** Call `wordWrapBreak` repeatedly to find sub-line
  boundaries. Skip the first `skip_sublines` sub-lines of the `rowoff` row
  (see §5.3). Render each visible sub-line, padding with spaces to the
  screen width.

**`renderLineWithHighlighting`** is the inner rendering function. It walks
bytes in `chars`, expands tabs to spaces, control characters to `^X` notation,
and passes through UTF-8 sequences. It emits CSI 7m / CSI 0m for region
highlighting based on pre-computed column bounds.

### 4.3 Highlight Computation

Before rendering each logical row, `computeRowHighlightBounds` determines
the display-column range that should be highlighted (for the active region
and/or search match). This is computed once per row and checked with simple
integer comparisons in the per-column rendering loop, avoiding repeated
per-column region membership tests.

## 5. Scrolling and Word Wrap

### 5.1 Coordinate Systems

The editor operates in three coordinate systems:

1. **Buffer coordinates** `(cx, cy)` — byte offset within a logical line,
   logical line index. This is the canonical cursor position.
1. **Display coordinates** `(column, row)` — column position accounting for
   tab expansion and wide characters, logical line index. Computed by
   `charsToDisplayColumn`.
1. **Screen coordinates** `(scx, scy)` — column and row within a window,
   relative to the window’s top-left corner. Computed by `setScxScy`.

Word wrap introduces a sub-coordinate: a display position falls on a
specific **sub-line** within its logical row. `cursorScreenLine` computes
`(sub_line, sub_col)` from a display column.

### 5.2 The Screen Line Cache

The screen line cache maps logical rows to cumulative screen line numbers:

```
screen_line_start[i] = total screen lines occupied by rows 0..i-1
```

This enables O(1) lookup of a row’s screen position. The cache is:

- **Built** by `buildScreenCache`, which walks all rows calling
  `countScreenLines` only for rows whose `cached_width` is stale (`-1`),
  and reusing the cached value for rows that haven’t changed.
- **Invalidated** per-row: editing a row sets that row’s `cached_width`
  to `-1` and marks `screen_line_cache_valid = 0`.
- **Rebuilt** once at the start of each `refreshScreen`, before `scroll()`
  runs.

The cache is per-buffer, not per-window. Since rendering is read-only (§2.1),
all windows displaying the same buffer share the same cache without conflict.
Invalidating the cache affects all such windows, but the rebuild happens once
per frame regardless of window count.

**Per-row invalidation** is essential for viability. `rowInsertChar`,
`rowDelChar`, `rowAppendString`, and `editorInsertRow`/`editorDelRow` set
`cached_width = -1` only on the affected row(s) and set
`screen_line_cache_valid = 0` on the buffer. The expensive
`countScreenLines` call runs only for the modified row; unmodified rows
contribute only an integer addition to the cumulative sum.

Screen resize sets all rows’ `cached_width` to `-1` since the screen width
has changed, forcing a full recomputation. This is infrequent and acceptable.

### 5.3 Scroll Logic (Word Wrap)

When word wrap is enabled, a logical line may occupy multiple screen lines.
The scroll system must handle:

- The cursor moving to a sub-line that is off-screen.
- The top of the window falling in the middle of a logical line.

**`rowoff`** always refers to a logical line — the first (possibly partially)
visible line. **`skip_sublines`** is a per-frame derived value: the number
of sub-lines of the `rowoff` line that are above the visible area. It is
computed by `scroll()` and passed to `drawRows()`, which skips that many
sub-lines before rendering.

`skip_sublines` is not stored persistently. It is derived each frame from
`(rowoff, cursor position, window height)` and the screen line cache. This
means no cross-window synchronisation is needed, no state can become stale,
and the value is always consistent with the cache that was just built.

**Scroll algorithm:**

```
cursor_screen_line = screen_line_start[cy] + cursor_sub_line
rowoff_screen_line = screen_line_start[rowoff]

if cursor above window:
    rowoff = cy
    skip_sublines = 0

if cursor below window:
    target_top = cursor_screen_line - window_height + 1
    find rowoff such that screen_line_start[rowoff] <= target_top
    skip_sublines = target_top - screen_line_start[rowoff]

otherwise:
    skip_sublines = existing value (0 if rowoff was set by scrolling up)
```

### 5.4 Scroll Logic (No Wrap)

Without word wrap, each logical line is one screen line. `rowoff` and
`coloff` define the visible rectangle. No sub-line logic is needed.

### 5.5 Long Lines

Lines are stored as flat byte arrays. Inserting or deleting in the middle of
a line is O(line_length) due to `memmove`. For typical text files (lines
under a few hundred bytes), this is negligible.

For very long lines (thousands of bytes), the cost is:

- **Edit:** O(line_length) for the memmove. Unavoidable with a flat array.
- **Wrap computation:** O(line_length) per call to `countScreenLines`. Cached
  per row, so this runs once per edit to that row, not once per frame.
- **Rendering:** O(visible_portion) per frame. `renderLineWithHighlighting`
  skips to the start column, so only the visible sub-lines are processed.

A line length limit (currently 1MB) provides a safety valve. Gap buffers or
piece tables within a line are not justified for the expected use cases.

## 6. Undo / Redo

### 6.1 Architecture

The undo system uses a singly-linked stack of delta records. Each record
represents a contiguous text change:

```c
struct editorUndo {
    struct editorUndo *prev;
    int startx, starty;     /* start of affected range */
    int endx, endy;         /* end of affected range */
    int delete;             /* 1 = deletion, 0 = insertion */
    int paired;             /* linked to next record (for compound ops) */
    int append;             /* can coalesce with next keystroke */
    int datalen, datasize;
    uint8_t *data;          /* the text that was inserted or deleted */
};
```

**Data format:** `data` always contains the affected text in **forward
order** — matching the text as it appeared (or would appear) in the buffer
from `(startx, starty)` to `(endx, endy)`. This is valid UTF-8.

**Two stacks:** `buf->undo` is the undo stack (most recent at top).
`buf->redo` is the redo stack. Performing an undo pops from undo, applies
the inverse operation, and pushes the record onto redo. A new edit clears
the redo stack.

Because only one buffer is mutated at a time (§2.1), there is no risk of
interleaved undo records from different buffers.

### 6.2 Primitive / Wrapper Split

Buffer manipulation is split into two layers:

|Layer                   |Functions                                                                                                     |Records undo?|
|------------------------|--------------------------------------------------------------------------------------------------------------|-------------|
|**Raw primitives**      |`editorInsertNewlineRaw`, `editorInsertChar`, `rowInsertChar`, `rowDelChar`, `editorInsertRow`, `editorDelRow`|No           |
|**User-facing wrappers**|`editorInsertNewline`, `editorBackSpace`, `editorDelChar`, `editorInsertNewlineAndIndent`, etc.               |Yes          |

The wrappers call the appropriate undo-recording function, then call the
raw primitive. This separation exists so that:

- **Undo/redo replay** calls raw primitives directly. No undo records are
  created during replay. No flags or suppression modes are needed.
- **Operations with manual undo records** (yank, kill-region, transforms)
  call raw primitives and construct their own undo records explicitly.
- **Future code** that needs to manipulate the buffer without recording undo
  has a clean, obvious path.

### 6.3 Recording

**Insertion recording:** `editorUndoAppendChar` is called before each
character insertion. Consecutive characters at adjacent positions are
coalesced into a single record (the `append` flag).

**Deletion recording:**

- `editorUndoBackSpace` — records bytes deleted by backspace. Since
  backspace moves the cursor left, bytes arrive in reverse order.
  They are **prepended** to `data` so that the result is in forward
  (file) order.
- `editorUndoDelChar` — records bytes deleted by forward-delete.
  Bytes are **appended** to `data` in their natural order.

**Compound operations:** Kill-region, yank, and transformations create
explicit undo records with the full text, bypassing the incremental
recording functions. Transformations use `paired` records to link the
delete and re-insert as a single undoable unit.

### 6.4 Replay

Both undo and redo replay use **bulk buffer operations** — direct `memmove`
and `memcpy` on row data, `editorInsertRow` for new lines, `editorDelRow`
for line removal. Neither path uses character-at-a-time insertion.

**Undoing an insertion** (record has `delete == 0`): Delete the text from
`(startx, starty)` to `(endx, endy)`. If the range is within a single row,
a single `memmove` removes the bytes. If the range spans multiple rows,
delete the interior rows with `editorDelRow`, then join the first and last
rows by splicing their `chars` arrays.

**Undoing a deletion** (record has `delete == 1`): Re-insert the text from
`data` starting at `(startx, starty)`. This is the inverse of the deletion
algorithm:

- **Single-line data** (no `\n` in `data`): `memmove` the tail of the row
  right by `datalen` bytes, then `memcpy` the data into the gap.
- **Multi-line data:** Split the current row at `startx`. The bytes before
  `startx` become the prefix; the bytes after `startx` become the suffix.
  Append the first line fragment from `data` to the prefix row. Insert
  complete interior lines from `data` as new rows via `editorInsertRow`.
  Prepend the last line fragment from `data` to the suffix row.

This ensures that both insertion and deletion replay are O(affected_text)
with a constant number of `memmove`/`memcpy` operations.

**Redo** mirrors undo: redoing an insertion re-inserts using the bulk
algorithm; redoing a deletion re-deletes using bulk deletion.

### 6.5 Coalescing Limit

Records are capped at `UNDO_LIMIT` (1000). When exceeded, the oldest
record is pruned from the tail of the undo stack.

## 7. Multi-Window Coordination

### 7.1 Cursor Ownership Protocol

The buffer owns the canonical cursor position `(cx, cy)`. All editing
operations read and write `buf->cx` and `buf->cy`. Each window also has
`win->cx` and `win->cy` fields, which serve as **snapshots** of the
buffer cursor — saved when the window loses focus and restored when it
regains focus.

The synchronisation protocol, implemented by `editorSwitchWindow`:

1. **Outgoing window:** Copy `buf->cx, buf->cy` into `win->cx, win->cy`.
1. **Incoming window:** Copy `win->cx, win->cy` into `buf->cx, buf->cy`.
   Then **clamp** to valid bounds via `synchronizeBufferCursor`: if
   `win->cy >= buf->numrows`, set to `numrows - 1` (or 0 if empty); if
   `win->cx > row[cy].size`, set to `row[cy].size`.

Clamping handles the case where edits from another window have shortened
or deleted lines since the snapshot was taken. This is the only case where
stale cursor data can arise, and it is addressed at the single point of
focus transfer.

**Invariant:** Between window switches, only the focused window’s cursor is
live. Non-focused window snapshots may be stale but are never dereferenced
until the window is focused, at which point they are clamped.

### 7.2 Window Safety After Buffer Edits

When a buffer is edited, all windows displaying that buffer will show the
updated content on the next `refreshScreen`. Non-focused windows may have a
stale `rowoff` that points past the end of the buffer.

Because windows are passive views (§2.2) and only one buffer mutates at a
time (§2.1), exactly two cases arise:

**(a) Window shows the same buffer as the focused window.** Its `rowoff` may
have become invalid. The mandatory clamp handles this.

**(b) Window shows a different buffer.** Its buffer is unmodified, so its
`rowoff` is still valid. No work is needed.

**Mandatory bounds clamp:** At the start of each `refreshScreen`, before
any rendering occurs, every window’s `rowoff` is clamped:

```c
void clampAllWindows(void) {
    for (int i = 0; i < E.nwindows; i++) {
        struct editorWindow *win = E.windows[i];
        struct editorBuffer *buf = win->buf;
        if (buf->numrows == 0) {
            win->rowoff = 0;
        } else if (win->rowoff >= buf->numrows) {
            win->rowoff = buf->numrows - 1;
        }
    }
}
```

This is one comparison per window per frame and eliminates the entire class
of stale-rowoff crashes.

### 7.3 Rendering Multiple Windows

`refreshScreen` iterates windows top-to-bottom. For each window:

1. Clamp `rowoff` (already done by `clampAllWindows`).
1. If focused, run `scroll()` to update `rowoff` and derive `skip_sublines`.
1. Call `drawRows()` with the window’s height and buffer.
1. Call `drawStatusBar()` at the cumulative line position.

Because the layout is a full-width row stack (§2.3), each window’s rendering
is independent: it writes to a contiguous range of terminal rows with no
column clipping or horizontal overlap.

## 8. Deferred Optimisations

The following optimisations are identified but explicitly deferred until the
core architecture is stable and tested. They can be implemented independently
and incrementally.

The system model (§2) makes the viability of these optimisations clear:
because only one buffer mutates per edit, and the screen is a row stack with
no horizontal overlap, each optimisation can be applied to a specific window’s
row range without affecting others.

### 8.1 Per-Buffer Redraw Scoping

When a content change occurs (§2.4), only windows displaying the mutated
buffer need any content update. Windows displaying other buffers can be
skipped entirely. In the typical case (one or two windows on the same buffer),
this provides no savings, but it becomes meaningful with multiple buffers open.

### 8.2 Render Hints (Whitelist Optimisation)

Once the stateless renderer is proven correct, specific operations can
provide hints to skip unnecessary work:

- **Cursor-only movement** (no scroll change): Reposition terminal cursor
  only. No content redraw.
- **Single-character insertion** (no wrap height change): Redraw only the
  affected row. If the row’s screen line count didn’t change, use CSI K and
  rewrite just that row.
- **Vertical scroll by N lines** (content shifts but doesn’t change): Use
  CSI scroll region commands to shift existing content and render only the
  newly exposed lines.

These are pure optimisations. The system must produce correct output without
any hints — hints only allow skipping work that would produce identical
output.

### 8.3 Terminal Scroll Regions

VT100 supports scroll regions (CSI r) and index/reverse-index for scrolling
within them. Because windows are contiguous full-width row ranges (§2.3),
scroll regions map directly to windows with no coordinate translation. This
can avoid redrawing the entire window when scrolling by a small number of
lines. However, behaviour varies across terminal emulators, so this should
be gated behind a capability check or user setting.

### 8.4 Render Buffer as Optional Acceleration

If profiling reveals that on-the-fly tab/control expansion is a bottleneck
for specific workloads (unlikely for typical text, possible for files with
many tabs or control characters), a per-row render cache could be
reintroduced as an **optional acceleration layer** — computed lazily, used
when valid, but never required for correctness.

## 9. Summary of Changes from Current Implementation

|Area                |Current                                                                                                            |Proposed                                                                                |
|--------------------|-------------------------------------------------------------------------------------------------------------------|----------------------------------------------------------------------------------------|
|`erow`              |8 fields including `render`, `rsize`, `renderwidth`, `render_valid`, `width_valid`                                 |3 fields: `chars`, `size`, `cached_width` (sentinel: -1)                                |
|Rendering           |`drawRows` calls `updateRow` to populate render buffer, then `renderLineWithHighlighting` reads from `chars` anyway|`drawRows` reads directly from `chars`. No render buffer.                               |
|`updateRow`         |Builds render buffer with tab expansion and control char decoration                                                |Removed. Expansion happens in `renderLineWithHighlighting`.                             |
|`editorUpdateBuffer`|Marks all rows render-invalid                                                                                      |Removed or reduced to cache invalidation only.                                          |
|Cache invalidation  |Global: clears `width_valid` on every row                                                                          |Per-row: only the modified row’s `cached_width` set to -1                               |
|`buildScreenCache`  |Recomputes `countScreenLines` for every row                                                                        |Recomputes only rows with `cached_width == -1`; reuses cached values for unmodified rows|
|Scroll (wrap)       |Direct `countScreenLines` loop, backward walk for `rowoff`                                                         |Cache-based lookup, deterministic `target_top` search, derived `skip_sublines`          |
|Sub-line offset     |Not supported; always renders from sub-line 0 of `rowoff`                                                          |`skip_sublines` derived per frame, passed to `drawRows`                                 |
|Window safety       |No bounds check on `rowoff` before rendering                                                                       |Mandatory `rowoff` clamp at start of each frame                                         |
|Undo recording      |Mixed into buffer primitives                                                                                       |Split: raw primitives (no undo) + wrappers (record undo)                                |
|Undo data format    |Reversed byte order for delete records                                                                             |Forward order, valid UTF-8                                                              |
|Undo replay         |Character-at-a-time insertion via `editorInsertNewline`/`editorInsertChar`                                         |Bulk `memmove`/`memcpy` and `editorInsertRow` for both insertion and deletion replay    |
|Cursor ownership    |Implicit, easy to get wrong                                                                                        |Explicit protocol: buffer owns live cursor, windows hold snapshots, clamp on focus      |

## 10. Implementation Order

1. **Per-row cache invalidation and cache-based scroll** — Replace global
   `invalidateScreenCache` with per-row `cached_width = -1`. Update
   `buildScreenCache` to skip rows with valid caches. Rewrite `scroll()` to
   use `getScreenLineForRow`. Add `buildScreenCache` call at the start of
   `refreshScreen`. Add mandatory `rowoff` clamp for all windows. These
   changes are interdependent and must ship together.
1. **Remove render buffer** — Delete `updateRow`, `render`, `rsize`,
   `renderwidth`, `render_valid` from `erow`. Remove `editorUpdateBuffer` or
   reduce it to cache invalidation. Remove the
   `if (!row->render_valid) updateRow(row)` guard in `drawRows`. Audit
   `calculateLineWidth` to ensure it works without calling `updateRow`.
1. **Implement `skip_sublines`** — Compute in `scroll()`, pass to `drawRows`,
   skip the appropriate number of sub-lines when rendering the `rowoff` row.
1. **Bulk undo replay** — Replace character-at-a-time insertion in
   `editorDoUndo` and `editorDoRedo` with direct `memmove`/`memcpy` bulk
   operations.

**Render hints** — Deferred.
