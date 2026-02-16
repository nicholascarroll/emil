#ifndef ABUF_H
#define ABUF_H

/* Append buffer for efficient screen updates */
struct abuf {
	char *b;
	int len;
	int capacity;
};

#define ABUF_INIT { NULL, 0, 0 }

void abAppend(struct abuf *ab, const char *s, int len);
void abFree(struct abuf *ab);

#endif /* ABUF_H */
