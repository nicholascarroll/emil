#include "adjust.h"
#include "emil.h"

extern struct editorConfig E;

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

void adjustAllPoints(struct editorBuffer *buf, int startx, int starty, int endx,
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
	for (int i = 0; i < E.nwindows; i++) {
		if (!E.windows[i]->focused && E.windows[i]->buf == buf)
			adjustPoint(&E.windows[i]->cx, &E.windows[i]->cy,
				    startx, starty, endx, endy, is_delete);
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
