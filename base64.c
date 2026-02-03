#include "base64.h"
#include <stdlib.h>

static const char b64table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const uint8_t *src, size_t srclen) {
	size_t outlen = 4 * ((srclen + 2) / 3);
	char *out = malloc(outlen + 1);
	if (out == NULL)
		return NULL;

	size_t i = 0;
	size_t j = 0;

	while (i + 2 < srclen) {
		uint32_t triple = ((uint32_t)src[i] << 16) |
				  ((uint32_t)src[i + 1] << 8) |
				  ((uint32_t)src[i + 2]);
		out[j++] = b64table[(triple >> 18) & 0x3F];
		out[j++] = b64table[(triple >> 12) & 0x3F];
		out[j++] = b64table[(triple >> 6) & 0x3F];
		out[j++] = b64table[triple & 0x3F];
		i += 3;
	}

	if (i < srclen) {
		uint32_t triple = (uint32_t)src[i] << 16;
		if (i + 1 < srclen)
			triple |= (uint32_t)src[i + 1] << 8;

		out[j++] = b64table[(triple >> 18) & 0x3F];
		out[j++] = b64table[(triple >> 12) & 0x3F];
		out[j++] = (i + 1 < srclen) ? b64table[(triple >> 6) & 0x3F] :
					      '=';
		out[j++] = '=';
	}

	out[j] = '\0';
	return out;
}
