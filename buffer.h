/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#ifndef EMIL_BUFFER_H
#define EMIL_BUFFER_H
#include "emil.h"
#include "wrap.h"

/* Read-only guard: if buf is read-only, post "buffer read-only" to the
 * status bar and return non-zero; otherwise return zero.  Callers that
 * mutate the buffer should early-return when this returns non-zero.
 * e.g.    if (rejectIfReadOnly(buf)) return;
 */
struct buffer;
int rejectIfReadOnly(struct buffer *buf);

void insertRow(struct buffer *bufr, int at, const uint8_t *s, size_t len);
void appendRowRaw(struct buffer *bufr, const uint8_t *s, size_t len);
int killBufferNeedsConfirm(const struct buffer *bufr);
void rowEnsureCap(erow *row, int needed);
void freeRow(erow *row);
void delRow(struct buffer *bufr, int at);
void rowInsertChar(struct buffer *bufr, erow *row, int at, int c);
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

/* ---- Row-count invariant ----
 *
 *     numrows >= 1   and   cy < numrows
 *
 * A buffer of N rows represents the byte string
 *
 *     row[0] "\n" row[1] "\n" ... "\n" row[N-1]
 *
 * with no terminator, so whether the file ended in a newline is
 * carried by whether a final empty row exists:
 *
 *     ""        <->  [""]
 *     "a"       <->  ["a"]
 *     "a\n"     <->  ["a", ""]
 *     "a\nb"    <->  ["a", "b"]
 *     "a\nb\n"  <->  ["a", "b", ""]
 *
 * The empty buffer is therefore the one-row buffer holding an empty
 * row, not the rowless buffer.  numrows == 0 is unreachable.
 *
 * A cursor byte offset is likewise legal only at the start of a
 * character or at end of line -- never inside a multibyte character.
 * clampPositions enforces that after every command, so the many places
 * that move a cursor need not each remember to snap.
 *
 * Both invariants hold at every public API boundary, not at every
 * instant.  Construction and whole-array reload may pass through zero
 * rows internally provided no caller can observe it; bufferResetRows
 * exists for exactly that and its callers must restore the invariant
 * before returning.  clearBuffer is likewise a low-level primitive
 * that leaves the buffer rowless -- its callers repopulate. */

struct buffer *findBufferByName(const char *name);
struct buffer *findOrCreateSpecialBuffer(const char *name);
void clearBuffer(struct buffer *buf);
void bufferResetRows(struct buffer *buf);
void bufferEnsureRow(struct buffer *buf);
int bufferLineCount(struct buffer *buf);
int bufferIsEmpty(struct buffer *buf);
void bufferEnsureFinalNewline(struct buffer *buf);
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
