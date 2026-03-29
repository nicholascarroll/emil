#include "abuf.h"
#include "terminal.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

void abAppend(struct abuf *ab, const char *s, int len) {
	if (ab->len + len > ab->capacity) {
		int new_capacity = ab->capacity == 0 ? 8192 : ab->capacity * 2;
		while (new_capacity < ab->len + len) {
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
