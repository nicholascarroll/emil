/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_DECODER_H
#define EMIL_DECODER_H

#include <stdint.h>

/*
 * Escape-sequence decoder: a pure state machine over a byte source.*/

/* Byte source.  Returns 1 with a byte in *out, or 0 if no byte will
 * come.  The wait class says what a signal means, not how long to
 * wait -- the source blocks in both cases:
 *   wait_indefinitely != 0  -> the byte after a raw ESC.  ESC is the
 *                              Meta prefix, so the source waits for
 *                              the user; 0 means the wait was
 *                              abandoned (e.g. a signal arrived).
 *   wait_indefinitely == 0  -> a byte inside a terminal-generated
 *                              sequence.  The source waits for it
 *                              without giving up on signals, so a
 *                              sequence split by a slow transport
 *                              still decodes; 0 means the input
 *                              stream ended. */
typedef int (*escByteSourceFn)(uint8_t *out, int wait_indefinitely);

/* Maximum bytes recorded for reporting an unrecognized sequence
 * (longer sequences are still consumed in full; only the report is
 * truncated). */
#define ESC_SEEN_MAX 12

/* Decode one escape sequence: everything after a raw ESC byte.
 *
 * Returns a key token (see keymap.h).  A return of 033 means the
 * sequence was unrecognized, malformed, or interrupted; it has been
 * consumed in full, and seen[0..*n_seen) holds its bytes for the
 * caller to report.  For every other return value *n_seen is 0.
 */
int decodeEscapeSequence(escByteSourceFn next, uint8_t *seen, int *n_seen);

#endif /* EMIL_DECODER_H */
