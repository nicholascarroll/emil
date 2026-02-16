#include "abuf.h"
#include "terminal.h"
#include "util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void abAppend(struct abuf *ab, const char *s, int len) {
	if (ab->len + len > ab->capacity) {
		int new_capacity = ab->capacity == 0 ? 1024 : ab->capacity * 2;
		while (new_capacity < ab->len + len) {
			if (new_capacity > INT_MAX / 2) {
				die("buffer size overflow");
			}
			new_capacity *= 2;
		}
		ab->b = xrealloc(ab->b, new_capacity);
		ab->capacity = new_capacity;
	}
	memcpy(&ab->b[ab->len], s, len);
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}
