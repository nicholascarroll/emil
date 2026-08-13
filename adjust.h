/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_ADJUST_H
#define EMIL_ADJUST_H 1

#include "emil.h"

/* Adjust a tracked point (px, py) after a buffer mutation.
 *
 * For insertions (is_delete == 0):
 *   The region [startx,starty] to [endx,endy] was inserted.
 * For deletions  (is_delete == 1):
 *   The region [startx,starty] to [endx,endy] was deleted.
 *
 * Returns 1 if the point was inside a deleted region (now clamped to
 * the deletion start), 0 otherwise.
 */
int adjustPoint(int *px, int *py, int startx, int starty, int endx, int endy,
		int is_delete);

/* Adjust all tracked points for a buffer after a mutation.
 * This adjusts:
 *   - the mark (markx/marky) if set
 *   - saved cursors of non-focused windows showing this buffer
 *   - the mark ring
 *   - all REGISTER_POINT entries referencing this buffer
 *   - rowoff of every window showing this buffer, focused or not,
 *     adjusted as a row index rather than a position, with
 *     skip_sublines zeroed where the row it counted against was
 *     deleted
 */
void adjustAllPoints(struct buffer *buf, int startx, int starty, int endx,
		     int endy, int is_delete);

#endif
