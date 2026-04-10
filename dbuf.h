#ifndef EMIL_DBUF_H
#define EMIL_DBUF_H 1

#include <stdint.h>

/* Dynamic byte buffer for building mutation text, replacement
 * strings, and subprocess output.  Owns its allocation.
 *
 * Usage:
 *   struct dbuf d = DBUF_INIT;
 *   dbuf_append(&d, data, len);
 *   dbuf_byte(&d, '\n');
 *   dbuf_pad(&d, ' ', count);
 *   uint8_t *result = dbuf_detach(&d, &out_len);
 *   // caller frees result
 *
 * All growth is overflow-checked: if the buffer would exceed
 * INT_MAX bytes, the process aborts via xmalloc/xrealloc. */

struct dbuf {
	uint8_t *buf;
	int len;
	int cap;
};

#define DBUF_INIT { NULL, 0, 0 }

/* Guarantee room for at least 'n' more bytes. */
void dbuf_ensure(struct dbuf *d, int n);

/* Append 'n' bytes from 'data'. */
void dbuf_append(struct dbuf *d, const uint8_t *data, int n);

/* Append a single byte. */
void dbuf_byte(struct dbuf *d, uint8_t c);

/* Append byte 'c' repeated 'n' times. */
void dbuf_pad(struct dbuf *d, uint8_t c, int n);

/* NUL-terminate, hand off ownership.  Sets *out_len to the byte
 * count (excluding NUL).  Resets the dbuf to empty.  Caller frees
 * the returned pointer. */
uint8_t *dbuf_detach(struct dbuf *d, int *out_len);

/* Free the buffer without detaching.  Use when the result is not
 * needed (error paths). */
void dbuf_free(struct dbuf *d);

#endif
