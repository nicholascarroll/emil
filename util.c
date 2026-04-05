#include "util.h"
#include "emil.h"
#include "fileio.h"
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <sys/types.h>

extern struct config E;

size_t totalOpenBytes(void) {
	size_t total = 0;
	for (struct buffer *b = E.headbuf; b; b = b->next)
		total += b->file_size;
	return total;
}

size_t totalKillBytes(void) {
	size_t total = 0;
	for (struct historyEntry *e = E.kill_history.head; e; e = e->next)
		total += strlen(e->str);
	return total;
}

void *xmalloc(size_t size) {
	void *ptr = malloc(size);
	if (!ptr && size != 0) {
		fprintf(stderr,
			"xmalloc: out of memory (allocating %zu bytes)\n",
			size);
		abort();
	}
	return ptr;
}

void *xrealloc(void *ptr, size_t size) {
	void *new_ptr = realloc(ptr, size);
	if (!new_ptr && size != 0) {
		fprintf(stderr,
			"xrealloc: out of memory (allocating %zu bytes)\n",
			size);
		abort();
	}
	return new_ptr;
}

void *xcalloc(size_t nmemb, size_t size) {
	void *ptr = calloc(nmemb, size);
	if (!ptr && nmemb != 0 && size != 0) {
		fprintf(stderr,
			"xcalloc: out of memory (allocating %zu * %zu bytes)\n",
			nmemb, size);
		abort();
	}
	return ptr;
}

char *xstrdup(const char *s) {
	size_t len = strlen(s) + 1;
	char *ptr = xmalloc(len);
	memcpy(ptr, s, len);
	return ptr;
}

ssize_t emil_getline(char **lineptr, size_t *n, FILE *stream) {
	char *ptr, *eptr;

	if (lineptr == NULL || n == NULL) {
		errno = EINVAL;
		return -1;
	}

	if (ferror(stream))
		return -1;

	if (feof(stream))
		return -1;

	if (*lineptr == NULL || *n == 0) {
		*n = 120;
		*lineptr = malloc(*n);
		if (*lineptr == NULL)
			return -1;
	}

	(*lineptr)[0] = '\0';

	/* Read first chunk */
	if (fgets(*lineptr, *n, stream) == NULL) {
		return -1;
	}

	/* Keep reading until we get a newline or EOF */
	while (1) {
		size_t len = strlen(*lineptr);

		if (len == 0)
			break;

		if ((*lineptr)[len - 1] == '\n')
			return len;

		/* Line doesn't end with newline, need to grow buffer and read more */
		*n *= 2;
		ptr = realloc(*lineptr, *n);
		if (ptr == NULL)
			return -1;
		*lineptr = ptr;

		eptr = *lineptr + len;
		if (fgets(eptr, *n - len, stream) == NULL)
			break;
	}

	return (*lineptr)[0] != '\0' ? (ssize_t)strlen(*lineptr) : -1;
}

size_t emil_strlcpy(char *dst, const char *src, size_t dsize) {
	const char *osrc = src;
	size_t nleft = dsize;

	/* Copy as many bytes as will fit. */
	if (nleft != 0) {
		while (--nleft != 0) {
			if ((*dst++ = *src++) == '\0')
				break;
		}
	}

	/* Not enough room in dst, add NUL and traverse rest of src. */
	if (nleft == 0) {
		if (dsize != 0)
			*dst = '\0'; /* NUL-terminate dst */
		while (*src++)
			;
	}

	return (src - osrc - 1); /* count does not include NUL */
}

size_t emil_strlcat(char *dst, const char *src, size_t dsize) {
	const char *odst = dst;
	const char *osrc = src;
	size_t n = dsize;
	size_t dlen;

	/* Find the end of dst and adjust bytes left but don't go past end. */
	while (n-- != 0 && *dst != '\0')
		dst++;
	dlen = dst - odst;
	n = dsize - dlen;

	if (n-- == 0)
		return (dlen + strlen(src));

	while (*src != '\0') {
		if (n != 0) {
			*dst++ = *src;
			n--;
		}
		src++;
	}
	*dst = '\0';

	return (dlen + (src - osrc)); /* count does not include NUL */
}

int isWordBoundary(uint8_t c) {
	return !(c > '~') && /* Anything outside ASCII is not a boundary */
	       !('a' <= c && c <= 'z') && /* Lower case ASCII not boundaries */
	       !('A' <= c && c <= 'Z') && /* Same with caps */
	       !('0' <= c && c <= '9') && /* And numbers */
	       ((c < '$') ||		  /* ctrl chars & some punctuation */
		(c > '%')); /* Rest of ascii outside $% & other ranges */
}

/* Expand a leading "~" or "~/" to $HOME.  Returns a new string
 * (caller frees).  If no expansion is needed, returns xstrdup(path). */
char *expandTilde(const char *path) {
	if (path[0] != '~')
		return xstrdup(path);
	if (path[1] != '\0' && path[1] != '/')
		return xstrdup(path);

	const char *home = getenv("HOME");
	if (!home)
		return xstrdup(path);

	size_t hlen = strlen(home);
	size_t tlen = strlen(path + 1);
	char *out = xmalloc(hlen + tlen + 1);
	memcpy(out, home, hlen);
	memcpy(out + hlen, path + 1, tlen + 1);
	return out;
}

/* If path starts with $HOME, replace that prefix with "~".
 * Returns a new string (caller frees). */
char *collapseHome(const char *path) {
	if (path[0] != '/')
		return xstrdup(path);

	const char *home = getenv("HOME");
	if (!home || home[0] != '/')
		return xstrdup(path);

	size_t hlen = strlen(home);
	while (hlen > 1 && home[hlen - 1] == '/')
		hlen--;

	if (strncmp(path, home, hlen) != 0)
		return xstrdup(path);
	if (path[hlen] != '\0' && path[hlen] != '/')
		return xstrdup(path);

	size_t tlen = strlen(path + hlen);
	char *out = xmalloc(1 + tlen + 1);
	out[0] = '~';
	memcpy(out + 1, path + hlen, tlen + 1);
	return out;
}

/* Resolve a path to absolute form for comparison purposes.
 * Normalizes . and .. segments.  Does NOT resolve symlinks.
 * Returns a new string; caller frees. */
char *absolutePath(const char *path) {
	if (!path || !*path)
		return xstrdup("");

	if (path[0] == '/') {
		char *out = xstrdup(path);
		cleanPath(out);
		return out;
	}

	if (path[0] == '~' && (path[1] == '\0' || path[1] == '/')) {
		char *out = expandTilde(path);
		cleanPath(out);
		return out;
	}

	char cwd[PATH_MAX];
	if (getcwd(cwd, sizeof(cwd)) == NULL)
		return xstrdup(path);

	size_t clen = strlen(cwd);
	size_t plen = strlen(path);
	char *out = xmalloc(clen + 1 + plen + 1);
	memcpy(out, cwd, clen);
	out[clen] = '/';
	memcpy(out + clen + 1, path, plen + 1);
	cleanPath(out);
	return out;
}
