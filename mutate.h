/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_MUTATE_H
#define EMIL_MUTATE_H 1

#include "emil.h"
#include <stdint.h>

/* WHEN TO USE mutate.h:
 * Use mutate.h for:
 *  - user-initiated range edits to a user-editable buffer that should be 
      undoable  and dirty the buffer
 * Use buffer.c primitives directly for:
 *  - file loading 
 *  - minibuffer/special/popup buffer population
 *  - inside the undo engine
*/

/* Collect buffer text from [startx,starty] to [endx,endy] into a
 * newly allocated NUL-terminated buffer.  Sets *out_len to the byte
 * count (excluding NUL).  Caller frees.
 * Assumes the range is normalised (start before end). */
uint8_t *collectRegionText(struct buffer *buf, int startx, int starty, int endx,
			   int endy, int *out_len);

enum mutateResult {
	MUTATE_DONE,
	MUTATE_REFUSED, /* read-only, or would delete only the final newline */
	MUTATE_INVALID, /* no such range: nothing changed, and reported */
};

/* Rebuilding a special buffer's contents (*Completions*, register
 * views, and so on): call Begin instead of bufferResetRows, fill the
 * rows, then call End.  A buffer with no history just gets its new rows.
 * One with history (made writable and edited) gets the change as an
 * ordinary replace, so its history still matches its text. */
struct mutateRebuild {
	uint8_t *old_text; /* NULL when there is no history */
	int old_len;
};
struct mutateRebuild mutateRebuildBegin(struct buffer *buf);
void mutateRebuildEnd(struct buffer *buf, struct mutateRebuild *r);

/* Delete text in [startx,starty]..[endx,endy], insert 'repl' at
 * (startx,starty).  Either the delete range or repl may be empty
 * (pure insert or pure delete).  Records a paired undo
 * (delete + insert) when both are non-empty, or a single undo
 * when one side is empty.
 *
 * The deleted text is read from the buffer here, so the record always
 * holds what the range held.
 *
 * 'repl' / 'repl_len' is the replacement text.  May be NULL/0 for
 * a pure delete.
 *
 * 'chain_to_prev': when non-zero, the first record pushed by this
 * call gets paired=1, chaining it to whatever mutation was pushed
 * immediately before.
 *
 * Refuses read-only buffers before any side effect (including
 * clearRedos): this is the authoritative check for the mutation
 * layer.  On that refusal, *out_endx and *out_endy are NOT written.
 *
 * The range must exist and start must not be after end, or the result
 * is MUTATE_INVALID.  Then, as when refusing to delete only the final
 * newline, nothing changes and the out-params get the current cursor,
 * so a caller's "move to the end of the edit" leaves it where it was.
 *
 * Calls clearRedos, records undo, performs mutation via
 * bulkInsert/bulkDelete, calls adjustAllPoints (inside bulk ops),
 * sets buf->dirty, calls updateBuffer.
 *
 * Does NOT set cursor, mark, or kill ring.
 *
 * Returns the end position of the inserted text in *out_endx,
 * *out_endy (may be NULL if caller doesn't need them). */
enum mutateResult mutateReplace(struct buffer *buf, int startx, int starty,
				int endx, int endy, const uint8_t *repl,
				int repl_len, int chain_to_prev, int *out_endx,
				int *out_endy);

/* Convenience: pure delete. */
enum mutateResult mutateDelete(struct buffer *buf, int startx, int starty,
			       int endx, int endy);

/* Convenience: pure insert. */
enum mutateResult mutateInsert(struct buffer *buf, int startx, int starty,
			       const uint8_t *text, int len, int *out_endx,
			       int *out_endy);

/* Single interactive edits — one keystroke's worth.  Identical to
 * mutateInsert / mutateDelete except that the record produced may be
 * folded into the run at the head of the undo list, so that typing
 * "hello" is one undo step rather than five.
 *
 * Use these only for edits the user performs one at a time.  Bulk
 * mutations (yank, kill, regex replace, insert-file) must use
 * mutateInsert / mutateDelete / mutateReplace, whose records always
 * stand alone. */
enum mutateResult mutateInsertChar(struct buffer *buf, int startx, int starty,
				   const uint8_t *text, int len, int *out_endx,
				   int *out_endy);
enum mutateResult mutateDeleteChar(struct buffer *buf, int startx, int starty,
				   int endx, int endy);

/* Append 'n_rows' empty lines to the end of buf.  Records a single
 * pure-insert undo with paired=0.  Used by yankRectangle to extend
 * the buffer before pasting a rectangle that overruns it — the
 * following mutateReplace passes chain_to_prev=1 to fold this into
 * a single atomic undo unit.
 *
 * 'from_row' is the row count before extension (buf->numrows at the
 * time of the call); passed explicitly so the caller's pre-state is
 * unambiguous.  Anything else is MUTATE_INVALID. */
enum mutateResult mutateExtendRows(struct buffer *buf, int from_row,
				   int n_rows);

#endif
