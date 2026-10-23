/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#include "abuf.h"
#include "terminal.h"
#include "util.h"
#include <limits.h>
#include <stdlib.h>
#include <string.h>

void abAppend(struct abuf *ab, const char *s, int len) {
	if (len < 0 || ab->len > INT_MAX - len)
		abort();
	if (ab->len + len > ab->capacity) {
		int need = ab->len + len;
		int new_capacity = ab->capacity == 0 ? 8192 : ab->capacity;
		while (new_capacity < need && new_capacity <= INT_MAX / 2)
			new_capacity *= 2;
		if (new_capacity < need)
			new_capacity = need;
		ab->b = xrealloc(ab->b, new_capacity);
		ab->capacity = new_capacity;
	}
	memcpy(&ab->b[ab->len], s, len);
	ab->len += len;
}

void abFree(struct abuf *ab) {
	free(ab->b);
}
