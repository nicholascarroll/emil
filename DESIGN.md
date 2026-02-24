# emil — High-Level Design

## 1. Purpose and Scope

This document describes the internal architecture of emil, a small terminal
text editor for UTF-8 files on VT100-compatible terminals. It covers buffer
management, rendering, scrolling, undo/redo, and multi-window coordination.

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
  logical line may span multiple screen lines (called "sub-lines"), but the
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


## 2. Buffer Architecture

### 2.1 The `erow` Structure

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
width of `0` columns — using zero as a sentinel would conflate "stale"
with "empty line."

**What was removed and why:**

- `render` / `rsize` / `renderwidth` / `render_valid` — The render buffer was
  a pre-expanded copy of `chars` with tabs converted to spaces and control
  characters decorated with escape sequences. It is no longer needed because
  `renderLineWithHighlighting` already reads directly from `chars` and expands
  tabs and control characters on the fly into the output `abuf`. The render
  buffer was vestigial: allocated, filled, and invalidated on every edit, but
  not consumed by the word-wrap rendering path.

- `width_valid` — Replaced by the `cached_width == -1` sentinel. A single
  field serves both purposes, reducing `erow` to three fields.

**Single source of truth:** `chars` is the only place text exists. No secondary
buffers are maintained for display purposes.

### 2.2 The `editorBuffer` Structure

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

    /* Screen line cache (see §4.2) */
    int *screen_line_start;
    int screen_line_cache_size;
    int screen_line_cache_valid;

    /* ... other fields (filename, mark, completion, etc.) */
};
```

The buffer owns the canonical cursor position `(cx, cy)`. See §6.1 for
how this interacts with multi-window cursor snapshots.

### 2.3 The `editorWindow` Structure

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
`rowoff` and `coloff` independently.


## 3. Rendering

### 3.1 Philosophy

Rendering is **stateless and single-pass**. On each frame, the renderer reads
raw bytes from `chars` and emits terminal-ready sequences directly into a
disposable `abuf`. No intermediate render buffers exist. The `abuf` is written
to the terminal in a single `write()` call.

This trades CPU cycles (re-expanding tabs and control characters each frame)
for architectural simplicity: there is no render state to invalidate, no
cache coherency to maintain between edits and display, and no class of bugs
where the render buffer is out of sync with `chars`.

### 3.2 The Rendering Pipeline

Each frame executes:

```
refreshScreen()
  ├── clampAllWindows()  — bounds-check rowoff for every window (see §6.3)
  ├── buildScreenCache() for each visible buffer (if stale)
  ├── scroll()           — compute rowoff, coloff, skip_sublines
  ├── for each window:
  │     ├── drawRows()     — emit visible content into abuf
  │     └── drawStatusBar()
  ├── drawMinibuffer()
  ├── position cursor
  └── write(abuf) → terminal
```

**`refreshScreen`** hides the cursor, homes it, renders all windows and
chrome, positions the cursor for the focused window, shows the cursor, and
flushes the `abuf` in one write.

**`drawRows`** walks logical lines from `rowoff` forward. For each line:

- **Without word wrap:** Render the portion from `coloff` to
  `coloff + screencols`, expanding tabs and control characters on the fly.
  Emit CSI K to clear trailing content.

- **With word wrap:** Call `wordWrapBreak` repeatedly to find sub-line
  boundaries. Skip the first `skip_sublines` sub-lines of the `rowoff` row
  (see §4.3). Render each visible sub-line, padding with spaces to the
  screen width. The word wrap computation reads directly from `chars`.

**`renderLineWithHighlighting`** is the inner rendering function. It walks
bytes in `chars`, expands tabs to spaces, control characters to `^X` notation,
and passes through UTF-8 sequences. It emits CSI 7m / CSI 0m for region
highlighting based on pre-computed column bounds.

### 3.3 Highlight Computation

Before rendering each logical row, `computeRowHighlightBounds` determines
the display-column range that should be highlighted (for the active region
and/or search match). This is computed once per row and checked with simple
integer comparisons in the per-column rendering loop, avoiding repeated
per-column region membership tests.


## 4. Scrolling and Word Wrap

### 4.1 Coordinate Systems

The editor operates in three coordinate systems:

1. **Buffer coordinates** `(cx, cy)` — byte offset within a logical line,
   logical line index. This is the canonical cursor position.

2. **Display coordinates** `(column, row)` — column position accounting for
   tab expansion and wide characters, logical line index. Computed by
   `charsToDisplayColumn`.

3. **Screen coordinates** `(scx, scy)` — column and row within a window,
   relative to the window's top-left corner. Computed by `setScxScy`.

Word wrap introduces a sub-coordinate: a display position falls on a
specific **sub-line** within its logical row. `cursorScreenLine` computes
`(sub_line, sub_col)` from a display column.

### 4.2 The Screen Line Cache

The screen line cache maps logical rows to cumulative screen line numbers:

```
screen_line_start[i] = total screen lines occupied by rows 0..i-1
```

This enables O(1) lookup of a row's screen position. The cache is:

- **Built** by `buildScreenCache`, which walks all rows calling
  `countScreenLines` only for rows whose `cached_width` is stale (`-1`),
  and reusing the cached value for rows that haven't changed.
- **Invalidated** per-row: editing a row sets that row's `cached_width`
  to `-1` and marks `screen_line_cache_valid = 0`.
- **Rebuilt** once at the start of each `refreshScreen`, before `scroll()`
  runs. This ensures the cache is consistent for the entire frame.

**Per-row invalidation** is essential for the viability of this approach.
If every edit invalidated every row (as in the prior implementation), then
`buildScreenCache` would be O(total_rows × average_row_width) per
keystroke — unusable for large files. With per-row invalidation, the
expensive `countScreenLines` call runs only for the modified row. The
cumulative sum update from that row forward is O(rows_after_modified) but
touches only integer additions, not character scanning.

**Implementation:** `rowInsertChar`, `rowDelChar`, `rowAppendString`, and
`editorInsertRow`/`editorDelRow` set `cached_width = -1` only on the
affected row(s) and set `screen_line_cache_valid = 0` on the buffer. The
current `invalidateScreenCache` function (which clears all rows) is removed.

When `buildScreenCache` runs and finds `screen_line_cache_valid == 0`, it
walks the row array: for rows with `cached_width >= 0`, it reuses the
cached value; for rows with `cached_width == -1`, it calls
`countScreenLines` and stores the result. It then recomputes the cumulative
`screen_line_start` array. A full walk of the cumulative array is
O(numrows) but avoids the O(row_width) `countScreenLines` call for
unmodified rows.

Screen resize sets all rows' `cached_width` to `-1` since the screen width
has changed, forcing a full recomputation. This is infrequent and acceptable.

### 4.3 Scroll Logic (Word Wrap)

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
means:

- No cross-window synchronisation is needed.
- No state can become stale between frames.
- The value is always consistent with the cache that was just built.

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

### 4.4 Scroll Logic (No Wrap)

Without word wrap, each logical line is one screen line. `rowoff` and
`coloff` define the visible rectangle. No sub-line logic is needed.

### 4.5 Long Lines

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


## 5. Undo / Redo

### 5.1 Architecture

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

### 5.2 Primitive / Wrapper Split

Buffer manipulation is split into two layers:

| Layer | Functions | Records undo? |
|-------|-----------|---------------|
| **Raw primitives** | `editorInsertNewlineRaw`, `editorInsertChar`, `rowInsertChar`, `rowDelChar`, `editorInsertRow`, `editorDelRow` | No |
| **User-facing wrappers** | `editorInsertNewline`, `editorBackSpace`, `editorDelChar`, `editorInsertNewlineAndIndent`, etc. | Yes |

The wrappers call the appropriate undo-recording function, then call the
raw primitive. This separation exists so that:

- **Undo/redo replay** calls raw primitives directly. No undo records are
  created during replay. No flags or suppression modes are needed.
- **Operations with manual undo records** (yank, kill-region, transforms)
  call raw primitives and construct their own undo records explicitly.
- **Future code** that needs to manipulate the buffer without recording undo
  has a clean, obvious path.

### 5.3 Recording

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

### 5.4 Replay

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
with a constant number of `memmove`/`memcpy` operations, regardless of the
size of the undo record. A kill-region of 10,000 characters followed by
undo does not incur 10,000 individual insert calls.

**Redo** mirrors undo: redoing an insertion re-inserts using the bulk
algorithm; redoing a deletion re-deletes using bulk deletion.

### 5.5 Coalescing Limit

Records are capped at `UNDO_LIMIT` (1000). When exceeded, the oldest
record is pruned from the tail of the undo stack.


## 6. Multi-Window Coordination

### 6.1 Cursor Ownership Protocol

The buffer owns the canonical cursor position `(cx, cy)`. All editing
operations read and write `buf->cx` and `buf->cy`. Each window also has
`win->cx` and `win->cy` fields, which serve as **snapshots** of the
buffer cursor — saved when the window loses focus and restored when it
regains focus.

The synchronisation protocol, implemented by `editorSwitchWindow`:

1. **Outgoing window:** Copy `buf->cx, buf->cy` into `win->cx, win->cy`.
   The window now holds a snapshot of where the cursor was.

2. **Incoming window:** Copy `win->cx, win->cy` into `buf->cx, buf->cy`.
   Then **clamp** to valid bounds: if `win->cy >= buf->numrows`, set it to
   `numrows - 1` (or 0 if empty); if `win->cx > row[cy].size`, set it to
   `row[cy].size`. This clamping (performed by `synchronizeBufferCursor`)
   handles the case where buffer edits from another window have shortened
   or deleted lines since the snapshot was taken.

**Invariant:** Between window switches, only the focused window's cursor is
live. Non-focused window snapshots may be stale but are never dereferenced
until the window is focused, at which point they are clamped.

### 6.2 Rendering Multiple Windows

`refreshScreen` iterates windows top-to-bottom. For each window:

1. Clamp `rowoff` to valid bounds (see §6.3).
2. If focused, run `scroll()` to update `rowoff`.
3. Call `drawRows()` with the window's height and buffer.
4. Call `drawStatusBar()` at the cumulative line position.

Non-focused windows do not run `scroll()` — their `rowoff` is fixed at
whatever it was when they lost focus.

### 6.3 Window Safety After Buffer Edits

When a buffer is edited, all windows displaying that buffer will show the
updated content on the next `refreshScreen`. However, non-focused windows
may have a stale `rowoff` that points past the end of the buffer — for
example, if lines were deleted above the viewport.

**This is a crash if not handled.** Accessing `buf->row[rowoff]` when
`rowoff >= buf->numrows` is an out-of-bounds read.

**Mandatory bounds clamp:** At the start of each `refreshScreen`, before
any rendering occurs, every window's `rowoff` is clamped:

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

This is cheap (one comparison per window per frame) and eliminates the
entire class of stale-rowoff crashes. It runs unconditionally, before the
screen line cache is built or any drawing begins.

**Screen line cache:** The cache is per-buffer, not per-window. Invalidating
the cache affects all windows displaying that buffer. The cache is rebuilt
once per frame in `refreshScreen`, before any window is drawn.


## 7. Deferred Optimisations

The following optimisations are identified but explicitly deferred until the
core architecture is stable and tested. They can be implemented independently
and incrementally.

### 7.1 Render Hints (Whitelist Optimisation)

Once the stateless renderer is proven correct, specific operations can
provide hints to skip unnecessary work:

- **Cursor-only movement** (no scroll change): Reposition terminal cursor
  only. No content redraw.
- **Single-character insertion** (no wrap height change): Redraw only the
  affected row. If the row's screen line count didn't change, use CSI K and
  rewrite just that row.
- **Vertical scroll by N lines** (content shifts but doesn't change): Use
  CSI scroll region commands to shift existing content and render only the
  newly exposed lines.

These are pure optimisations. The system must produce correct output without
any hints — hints only allow skipping work that would produce identical
output.

### 7.2 Terminal Scroll Regions

VT100 supports scroll regions (CSI r) and index/reverse-index for scrolling
within them. This can avoid redrawing the entire window when scrolling by a
small number of lines. However, behaviour varies across terminal emulators,
so this should be gated behind a capability check or user setting.

### 7.3 Render Buffer as Optional Acceleration

If profiling reveals that on-the-fly tab/control expansion is a bottleneck
for specific workloads (unlikely for typical text, possible for files with
many tabs or control characters), a per-row render cache could be
reintroduced as an **optional acceleration layer** — computed lazily, used
when valid, but never required for correctness.


## 8. Summary of Changes from Current Implementation

| Area | Current | Proposed |
|------|---------|----------|
| `erow` | 8 fields including `render`, `rsize`, `renderwidth`, `render_valid`, `width_valid` | 3 fields: `chars`, `size`, `cached_width` (sentinel: -1) |
| Rendering | `drawRows` calls `updateRow` to populate render buffer, then `renderLineWithHighlighting` reads from `chars` anyway | `drawRows` reads directly from `chars`. No render buffer. |
| `updateRow` | Builds render buffer with tab expansion and control char decoration | Removed. Expansion happens in `renderLineWithHighlighting`. |
| `editorUpdateBuffer` | Marks all rows render-invalid | Removed or reduced to cache invalidation only. |
| Cache invalidation | Global: clears `width_valid` on every row | Per-row: only the modified row's `cached_width` set to -1 |
| `buildScreenCache` | Recomputes `countScreenLines` for every row | Recomputes only rows with `cached_width == -1`; reuses cached values for unmodified rows |
| Scroll (wrap) | Direct `countScreenLines` loop, backward walk for `rowoff` | Cache-based lookup, deterministic `target_top` search, derived `skip_sublines` |
| Sub-line offset | Not supported; always renders from sub-line 0 of `rowoff` | `skip_sublines` derived per frame, passed to `drawRows` |
| Window safety | No bounds check on `rowoff` before rendering | Mandatory `rowoff` clamp at start of each frame |
| Undo recording | Mixed into buffer primitives (`editorInsertNewline` records undo) | Split: raw primitives (no undo) + wrappers (record undo) |
| Undo data format | Reversed byte order for delete records | Forward order, valid UTF-8 |
| Undo replay | Character-at-a-time insertion via `editorInsertNewline`/`editorInsertChar` | Bulk `memmove`/`memcpy` and `editorInsertRow` for both insertion and deletion replay |
| Cursor ownership | Implicit, easy to get wrong | Explicit protocol: buffer owns live cursor, windows hold snapshots, clamp on focus |


## 9. Implementation Order

1. **Undo fixes** — Primitive/wrapper split, forward-order data, UTF-8-aware
   replay. (Drafted; see BUGFIX_NOTES.md.)

2. **Per-row cache invalidation and cache-based scroll** — Replace global
   `invalidateScreenCache` with per-row `cached_width = -1`. Update
   `buildScreenCache` to skip rows with valid caches. Rewrite `scroll()` to
   use `getScreenLineForRow`. Add `buildScreenCache` call at the start of
   `refreshScreen`. Add mandatory `rowoff` clamp for all windows. These
   changes are interdependent and must ship together: the cache-based scroll
   is only viable if per-row invalidation keeps the rebuild cost
   proportional to the number of modified rows.

3. **Remove render buffer** — Delete `updateRow`, `render`, `rsize`,
   `renderwidth`, `render_valid` from `erow`. Remove `editorUpdateBuffer` or
   reduce it to cache invalidation. Remove the `if (!row->render_valid)
   updateRow(row)` guard in `drawRows`. Audit `calculateLineWidth` to ensure
   it works without calling `updateRow`.

4. **Implement `skip_sublines`** — Compute in `scroll()`, pass to `drawRows`,
   skip the appropriate number of sub-lines when rendering the `rowoff` row.

5. **Bulk undo replay** — Replace character-at-a-time insertion in
   `editorDoUndo` and `editorDoRedo` with direct `memmove`/`memcpy` bulk
   operations, matching the approach already used for the deletion path.
   This eliminates the O(n²) performance cliff for large undo records.

6. **Render hints** — Deferred optimisation. Add incrementally for specific
   operations after the base is stable.
