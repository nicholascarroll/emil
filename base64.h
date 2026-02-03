#ifndef EMIL_BASE64_H
#define EMIL_BASE64_H

#include <stddef.h>
#include <stdint.h>

/*
 * base64_encode - Encode binary data to Base64 string.
 *
 * Returns a newly allocated null-terminated string containing the
 * Base64-encoded representation of the input, or NULL on allocation
 * failure. Caller must free the returned string.
 *
 * src:     Input data.
 * srclen:  Length of input data in bytes.
 */
char *base64_encode(const uint8_t *src, size_t srclen);

#endif /* EMIL_BASE64_H */
