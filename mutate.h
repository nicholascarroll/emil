#ifndef EMIL_MUTATE_H
#define EMIL_MUTATE_H 1

#include "emil.h"
#include <stdint.h>

/* WHEN TO USE mutate.h:
 * Use mutate.h for:
 *  - user-initiated range edits to a user-editable buffer that should be undoable and dirty the buffer
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

/* Delete text in [startx,starty]..[endx,endy], insert 'repl' at
 * (startx,starty).  Either the delete range or repl may be empty
 * (pure insert or pure delete).  Records a paired undo
 * (delete + insert) when both are non-empty, or a single undo
 * when one side is empty.
 *
 * 'old_text' is the text being deleted.  Caller provides it
 * (collected before mutation).  Length is 'old_len'.
 *
 * 'repl' / 'repl_len' is the replacement text.  May be NULL/0 for
 * a pure delete.
 *
 * 'chain_to_prev' — when non-zero, the first record pushed by this
 * call gets paired=1, chaining it to whatever mutation was pushed
 * immediately before.  This is how yankRectangle gets a rectangle
 * paste with row-extension to undo atomically.  Default usage is 0.
 *
 * Calls clearRedos, records undo, performs mutation via
 * bulkInsert/bulkDelete, calls adjustAllPoints (inside bulk ops),
 * sets buf->dirty, calls updateBuffer.
 *
 * Does NOT set cursor, mark, or kill ring.
 *
 * Returns the end position of the inserted text in *out_endx,
 * *out_endy (may be NULL if caller doesn't need them). */
void mutateReplace(struct buffer *buf, int startx, int starty, int endx,
		   int endy, const uint8_t *old_text, int old_len,
		   const uint8_t *repl, int repl_len, int chain_to_prev,
		   int *out_endx, int *out_endy);

/* Convenience: pure delete. */
void mutateDelete(struct buffer *buf, int startx, int starty, int endx,
		  int endy, const uint8_t *old_text, int old_len);

/* Convenience: pure insert. */
void mutateInsert(struct buffer *buf, int startx, int starty,
		  const uint8_t *text, int len, int *out_endx, int *out_endy);

/* Append 'n_rows' empty lines to the end of buf.  Records a single
 * pure-insert undo with paired=0.  Used by yankRectangle to extend
 * the buffer before pasting a rectangle that overruns it — the
 * following mutateReplace passes chain_to_prev=1 to fold this into
 * a single atomic undo unit.
 *
 * 'from_row' is the row count before extension (buf->numrows at the
 * time of the call); passed explicitly so the caller's pre-state is
 * unambiguous. */
void mutateExtendRows(struct buffer *buf, int from_row, int n_rows);

#endif
