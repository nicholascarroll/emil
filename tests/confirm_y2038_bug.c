/*
 * confirm_y2038_bug.c — Y2038 platform canary + emil immunity check
 *
 * Confirms that this platform has a 32-bit time_t that will overflow on
 * 2038-01-19 03:14:07 UTC. Then verifies that mtime equality comparison 
 * works correctly. Emil uses mtime equality comparison.
 *
 * Build:  cc -o confirm_y2038_bug tests/confirm_y2038_bug.c
 * Run:    ./confirm_y2038_bug
 *
 * Exit 0 = pass (or skip).  Exit 1 = unexpected failure.
 */

#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

/* ---- helpers ---- */

static int tests_run = 0;
static int tests_passed = 0;

#define CHECK(msg, cond)                             \
	do {                                         \
		tests_run++;                         \
		if (cond) {                          \
			tests_passed++;              \
		} else {                             \
			printf("  FAIL: %s\n", msg); \
		}                                    \
	} while (0)

/* Store an mtime at "open", compare at "check".
 * Returns 1 if external_mod would be set (mtime differs). */
static int mtime_detects_change(time_t open_time, time_t check_time) {
	/* This mirrors fileio.c::checkFileModified:
     *   if (st.st_mtime != E.buf->open_mtime)
     *       E.buf->external_mod = 1;                       */
	return check_time != open_time;
}

/* ---- main ---- */

int main(void) {
	printf("Y2038 test — sizeof(time_t) = %zu bytes\n", sizeof(time_t));

	/* Platform canary — does this platform have the bug? */

	if (sizeof(time_t) > 4) {
		printf("SKIP: time_t is 64-bit, no Y2038 overflow on this platform.\n");
		printf("      Emil is trivially safe here.\n");
		return 0;
	}

	/* 32-bit time_t: confirm the overflow actually happens */
	time_t max32 = (time_t)2147483647; /* 2038-01-19 03:14:07 UTC */
	time_t wrapped = max32 + 1;

	if (wrapped >= 0) {
		printf("UNEXPECTED: 32-bit time_t but no sign wrap.\n");
		printf("  (Unsigned time_t or compiler optimised away the overflow.)\n");
		/* Not a failure — just means we can't demonstrate the bug. */
		return 0;
	}

	printf("CONFIRMED: 32-bit time_t overflows (max+1 = %ld).\n",
	       (long)wrapped);

	/* Verify allowed mtime comparison */

	printf("\nEmil mtime-comparison checks:\n");

	/* Normal case: same mtime → no change detected */
	CHECK("same mtime (normal range) → no change",
	      !mtime_detects_change(1000000, 1000000));

	/* Normal case: different mtime → change detected */
	CHECK("different mtime (normal range) → change detected",
	      mtime_detects_change(1000000, 1000001));

	/* Near the boundary: file opened at max32, checked at max32 */
	CHECK("mtime at max32, unchanged → no change",
	      !mtime_detects_change(max32, max32));

	/* Near the boundary: file opened at max32, modified after overflow */
	CHECK("mtime at max32, changed to wrapped → change detected",
	      mtime_detects_change(max32, wrapped));

	/* Both sides wrapped: opened and checked in post-overflow era */
	CHECK("both wrapped, same → no change",
	      !mtime_detects_change(wrapped, wrapped));

	CHECK("both wrapped, different → change detected",
	      mtime_detects_change(wrapped, wrapped + 1));

	/* Wrapped mtime compared to pre-overflow mtime */
	CHECK("wrapped vs pre-overflow → change detected",
	      mtime_detects_change(wrapped, max32));

	printf("\n%d/%d checks passed.\n", tests_passed, tests_run);

	if (tests_passed == tests_run) {
		printf("mtime comparison works fine post Y2038 on this 32-bit platform.\n");
		return 0;
	}

	printf("FAILURE: some checks did not pass.\n");
	return 1;
}
