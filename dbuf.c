#include "dbuf.h"
#include "util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void dbuf_ensure(struct dbuf *d, int n) {
	if (n <= 0)
		return;
	/* Overflow check: can we even represent len + n? */
	if (d->len > INT_MAX - n) {
		/* Pathological — abort cleanly via xmalloc(impossible). */
		xmalloc((size_t)INT_MAX + 1);
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
