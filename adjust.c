/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include "adjust.h"
#include "emil.h"

int adjustPoint(int *px, int *py, int startx, int starty, int endx, int endy,
		int is_delete) {
	int lines_delta = endy - starty;

	if (is_delete) {
		/* Point is before the deleted region — no change */
		if (*py < starty || (*py == starty && *px <= startx))
			return 0;

		/* Point is inside the deleted region — clamp to start */
		if (*py < endy || (*py == endy && *px <= endx)) {
			*px = startx;
			*py = starty;
			return 1;
		}

		/* Point is on the end line but after the deletion */
		if (*py == endy) {
			*px = startx + (*px - endx);
			*py = starty;
			return 0;
		}

		/* Point is after the deleted region */
		*py -= lines_delta;
		return 0;

	} else {
		/* Insertion */

		/* Point is strictly before the insertion — no change */
		if (*py < starty || (*py == starty && *px < startx))
			return 0;

		/* Point is on the insertion line */
		if (*py == starty) {
			if (lines_delta == 0) {
				/* Same-line insert: shift column right */
				*px += (endx - startx);
			} else {
				/* Multi-line insert starting on this line */
				*px = endx + (*px - startx);
				*py += lines_delta;
			}
			return 0;
		}

		/* Point is after the insertion line */
		*py += lines_delta;
		return 0;
	}
}

/* Adjust a tracked viewport top: a row index and the sub-line within
 * that row.  The row has no column, so it moves at row granularity: a
 * deletion spanning rows shifts it up by the rows removed and collapses
 * to the deletion's first row if it sat inside the span; an insertion
 * shifts it down.
 *
 * The sub-line is only ever zeroed, never renumbered.  Where the top
 * row survives the mutation its wrap points may move, but the walkers
 * clamp a sub-line past the row's last when they reach it, so nothing
 * needs recomputing here.  Where the top row does NOT survive the
 * sub-line has to go, which is the case below. */
static void adjustRowIndex(int *py, int *skip, int starty, int endy,
			   int is_delete) {
	int lines_delta = endy - starty;

	if (lines_delta == 0)
		return;

	if (is_delete) {
		if (*py <= starty)
			return;
		if (*py <= endy) {
			*py = starty; /* rows starty..endy became one */
			/* The skip counted sub-lines of a row that no
			 * longer exists; carried onto the surviving row
			 * it can name a sub-line that row does not have.
			 * drawRows() then skips past its last sub-line
			 * and emits nothing, leaving the window's top
			 * line blank.  scroll() would repair it, but
			 * scroll() runs for the focused window only, so
			 * an unfocused window keeps the blank line
			 * across a full frame. */
			*skip = 0;
		} else {
			*py -= lines_delta;
		}
	} else {
		if (*py > starty)
			*py += lines_delta;
	}
}

void adjustAllPoints(struct buffer *buf, int startx, int starty, int endx,
		     int endy, int is_delete) {
	/* Nothing to adjust if the mutation is a no-op */
	if (startx == endx && starty == endy)
		return;

	/* Adjust the mark */
	if (buf->markx >= 0 && buf->marky >= 0)
		adjustPoint(&buf->markx, &buf->marky, startx, starty, endx,
			    endy, is_delete);

	/* Adjust saved cursor positions in non-focused windows showing
	 * this buffer.  When the same buffer is displayed in multiple
	 * windows, the non-focused windows store a snapshot of cx/cy
	 * that becomes stale after mutations. */
	/* rowoff is adjusted for EVERY window on the buffer, including
	 * the focused one: the command moves the focused window's
	 * cursor, but nothing moves its viewport. */
	for (int i = 0; i < E.nwindows; i++) {
		if (E.windows[i]->buf != buf)
			continue;
		if (!E.windows[i]->focused)
			adjustPoint(&E.windows[i]->cx, &E.windows[i]->cy,
				    startx, starty, endx, endy, is_delete);
		adjustRowIndex(&E.windows[i]->rowoff,
			       &E.windows[i]->skip_sublines, starty, endy,
			       is_delete);
	}

	/* Adjust the mark ring.  Ring entries are byte offsets just
	 * like the live mark, so they go stale after every mutation
	 * unless adjusted here.  A stale entry restored by popMark()
	 * both jumps to the wrong place and can land mid-character,
	 * which breaks the buffer's valid-UTF-8 invariant once the
	 * user types there.
	 *
	 * The valid entries are exactly [0 .. mark_ring_len).  While
	 * the ring is not yet full, mark_ring_idx == mark_ring_len and
	 * pushes fill slots 0,1,2,... in order; once it is full every
	 * slot is valid.  popMark() rotates entries but changes
	 * neither index nor length, so the occupied set is unchanged.
	 *
	 * markRingPush() only ever stores a non-negative mark, but the
	 * guard mirrors the live-mark case above. */
	for (int m = 0; m < buf->mark_ring_len && m < MARK_RING_SIZE; m++) {
		if (buf->mark_ring[m].cx >= 0 && buf->mark_ring[m].cy >= 0)
			adjustPoint(&buf->mark_ring[m].cx,
				    &buf->mark_ring[m].cy, startx, starty, endx,
				    endy, is_delete);
	}

	/* Adjust register points for this buffer */
	for (int r = 0; r < 127; r++) {
		if (E.registers[r].rtype == REGISTER_POINT &&
		    E.registers[r].data.point.buf == buf) {
			adjustPoint(&E.registers[r].data.point.cx,
				    &E.registers[r].data.point.cy, startx,
				    starty, endx, endy, is_delete);
		}
	}
}
