#ifndef EMIL_BUFFER_H
#define EMIL_BUFFER_H
#include "emil.h"
#include "wrap.h"
void insertRow(struct buffer *bufr, int at, char *s, size_t len);
void freeRow(erow *row);
void delRow(struct buffer *bufr, int at);
void rowInsertChar(struct buffer *bufr, erow *row, int at, int c);
void rowInsertUnicode(struct buffer *bufr, erow *row, int at);
void rowAppendString(struct buffer *bufr, erow *row, char *s, size_t len);
void rowDelChar(struct buffer *bufr, erow *row, int at);
struct buffer *newBuffer(void);
void destroyBuffer(struct buffer *buf);
void updateBuffer(struct buffer *buf);
void switchToNamedBuffer(void);
void nextBuffer(void);
void previousBuffer(void);
void killBuffer(void);
void computeDisplayNames(void);
void clampToBuffer(struct buffer *buf, int *px, int *py);
void clampPositions(struct buffer *buf);

struct buffer *findBufferByName(const char *name);
struct buffer *findOrCreateSpecialBuffer(const char *name);
void clearBuffer(struct buffer *buf);
void closeSpecialBuffer(const char *name);
char *leftTruncate(const char *s, int max_width);
int nameFit(const char *name, int formatted_len);

/* Dirty-state transitions.  markBufferDirty acquires the advisory
 * file lock on the clean→dirty edge; markBufferClean releases it on
 * the dirty→clean edge.  Both are idempotent: calling markBufferDirty
 * on an already-dirty buffer, or markBufferClean on an already-clean
 * buffer, is a no-op. */
void markBufferDirty(struct buffer *buf);
void markBufferClean(struct buffer *buf);

#endif
