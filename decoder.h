#ifndef EMIL_DECODER_H
#define EMIL_DECODER_H

#include <stdint.h>

/*
 * Escape-sequence decoder: a pure state machine over a byte source.
 *
 * The decoder owns the grammar and the key mapping; it performs no
 * I/O, reads no editor state, and produces no messages.  The byte
 * source owns the clock: timeouts, signal handling, and blocking
 * policy live entirely behind escByteSourceFn.  This split is what
 * makes the machine unit-testable and fuzzable (tests/test_decoder.c
 * drives it from byte arrays) while the live editor drives it from
 * the terminal (terminal.c).
 */

/* Byte source.  Returns 1 with a byte in *out, or 0 if no byte is
 * available under the source's policy for the given wait class:
 *   wait_indefinitely != 0  -> the byte after a raw ESC.  ESC is the
 *                              Meta prefix, so the source should wait
 *                              for the user; 0 means the wait was
 *                              abandoned (e.g. a signal arrived).
 *   wait_indefinitely == 0  -> a byte inside a terminal-generated
 *                              sequence.  Terminals emit sequences
 *                              atomically, so the source should wait
 *                              only briefly; 0 means no sequence is
 *                              in flight (timeout). */
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
