/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#include "dbuf.h"
#include "util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void dbuf_ensure(struct dbuf *d, int n) {
	if (n <= 0)
		return;
	/* Overflow check: can we even represent len + n?
	 *
	 * abort(), not xmalloc((size_t)INT_MAX + 1) as this used to do.
	 * That was meant to "abort cleanly" by asking for an impossible
	 * allocation, but on 64-bit it is not impossible: measured here,
	 * a request one byte past INT_MAX SUCCEEDS.  So the guard leaked
	 * 2 GiB and then fell straight through into `int need = d->len +
	 * n` -- the signed overflow it exists to prevent -- while reading
	 * as fixed to anyone who greps for it.  A guard that commits the
	 * bug it guards against is worse than no guard, because it stops
	 * the next reader looking.
	 *
	 * Whether any live dbuf can actually reach INT_MAX is not
	 * established; the defect is real, its trigger is not
	 * demonstrated.  abort() is right either way: there is no
	 * recovery from a length that cannot be represented, and
	 * util.c's allocation guards already end the process the same
	 * way. */
	if (d->len > INT_MAX - n) {
		abort();
	}
	int need = d->len + n;
	if (need < d->cap)
		return;
	int new_cap = d->cap < 64 ? 64 : d->cap;
	while (new_cap < need) {
		if (new_cap > INT_MAX / 2) {
			new_cap = need;
			break;
		}
		new_cap *= 2;
	}
	d->buf = xrealloc(d->buf, new_cap);
	d->cap = new_cap;
}

void dbuf_append(struct dbuf *d, const uint8_t *data, int n) {
	if (n <= 0)
		return;
	dbuf_ensure(d, n);
	memcpy(&d->buf[d->len], data, n);
	d->len += n;
}

void dbuf_byte(struct dbuf *d, uint8_t c) {
	dbuf_ensure(d, 1);
	d->buf[d->len++] = c;
}

void dbuf_pad(struct dbuf *d, uint8_t c, int n) {
	if (n <= 0)
		return;
	dbuf_ensure(d, n);
	memset(&d->buf[d->len], c, n);
	d->len += n;
}

uint8_t *dbuf_detach(struct dbuf *d, int *out_len) {
	dbuf_byte(d, 0); /* NUL-terminate */
	d->len--;	 /* don't count NUL in length */
	uint8_t *result = d->buf;
	if (out_len)
		*out_len = d->len;
	d->buf = NULL;
	d->len = 0;
	d->cap = 0;
	return result;
}

void dbuf_free(struct dbuf *d) {
	free(d->buf);
	d->buf = NULL;
	d->len = 0;
	d->cap = 0;
}
