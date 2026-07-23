/* decoder.c: the escape-sequence state machine.
 *
 * Every byte after a raw ESC is decoded here.  States and
 * transitions:
 *
 *   ESC --'['--> CSI    body bytes 0x20..0x3F, one final 0x40..0x7E
 *   ESC --'O'--> SS3    exactly one final byte
 *   ESC --ESC--> ESC    a lone ESC keypress followed by the start of
 *                       a new sequence; the lone ESC is discarded so
 *                       the new sequence decodes normally instead of
 *                       leaking its body into the buffer as text
 *   ESC --0-9--> emit KEY_ALT_<digit>
 *   ESC --else-> emit KEY_META(byte)
 *
 * The wait class passed to the byte source encodes the timeout
 * policy (see decoder.h): indefinite for the byte after ESC, brief
 * for every byte after that, with these outcomes when no byte comes:
 *     CSI, nothing accumulated  -> KEY_META('[')  (Alt+[ was typed)
 *     SS3, nothing accumulated  -> KEY_META('O')  (Alt+Shift+O)
 *     mid-sequence              -> 033, bytes reported via seen[]
 *
 * Two invariants the machine maintains by construction, each the
 * site of a past field bug when it was maintained by hand: a
 * sequence's bytes are consumed exactly once (the accumulate loop is
 * also the drain; there is no shared drain with caller-dependent
 * preconditions), and no byte of a sequence is ever left queued to
 * be misread as typed text.
 */

#include "decoder.h"
#include "keymap.h"

/* Record a byte for reporting, silently truncating past the cap. */
static void note(uint8_t *seen, int *n, uint8_t b) {
	if (*n < ESC_SEEN_MAX)
		seen[(*n)++] = b;
}

/* A recognized key: discard the report bytes and return the token. */
static int dec_key(int *n_seen, int key) {
	*n_seen = 0;
	return key;
}

/* SS3 state (after ESC O): one final byte.  Sent by xterm-family
 * terminals for F1-F4 always, and for the cursor/Home/End keys when
 * a previous program left application cursor mode enabled. */
static int decodeSS3(escByteSourceFn next, uint8_t *seen, int *n_seen) {
	uint8_t b;
	if (next(&b, 0) != 1)
		return KEY_META('O'); /* Alt+Shift+O: nothing followed */
	switch (b) {
	case 'A':
		return KEY_ARROW_UP;
	case 'B':
		return KEY_ARROW_DOWN;
	case 'C':
		return KEY_ARROW_RIGHT;
	case 'D':
		return KEY_ARROW_LEFT;
	case 'H':
		return KEY_HOME;
	case 'F':
		return KEY_END;
	}
	note(seen, n_seen, 'O'); /* F1-F4 and the rest */
	note(seen, n_seen, b);
	return 033;
}

/* CSI state (after ESC [): accumulate body bytes until the final
 * byte, per the ECMA-48 grammar
 *     CSI  P..P  I..I  F
 * with parameter bytes 0x30..0x3F, intermediate bytes 0x20..0x2F,
 * and one final byte 0x40..0x7E.  The accumulate loop is also the
 * drain: an unmapped sequence has already been consumed in full by
 * the time it is reported, so nothing is left to leak. */
static int decodeCSI(escByteSourceFn next, uint8_t *seen, int *n_seen) {
	int body = 0; /* bytes accumulated after the '[' */
	note(seen, n_seen, '[');

	for (;;) {
		uint8_t b;
		if (next(&b, 0) != 1) {
			if (body == 0) {
				*n_seen = 0;
				return KEY_META('['); /* Alt+[ was typed */
			}
			return 033; /* incomplete sequence */
		}

		if (b >= 0x40 && b <= 0x7E) {
			/* Final byte: the sequence is complete and fully
			 * consumed; map it or report it. */
			if (body == 0) {
				switch (b) {
				case 'A':
					return dec_key(n_seen, KEY_ARROW_UP);
				case 'B':
					return dec_key(n_seen, KEY_ARROW_DOWN);
				case 'C':
					return dec_key(n_seen, KEY_ARROW_RIGHT);
				case 'D':
					return dec_key(n_seen, KEY_ARROW_LEFT);
				case 'F':
					return dec_key(n_seen, KEY_END);
				case 'H':
					return dec_key(n_seen, KEY_HOME);
				case 'Z':
					return dec_key(n_seen, KEY_BACKTAB);
				}
			} else if (body == 1 && b == '~' && seen[1] >= '0' &&
				   seen[1] <= '9') {
				switch (seen[1]) {
				case '1': /* vt220 Home */
				case '7': /* rxvt Home */
					return dec_key(n_seen, KEY_HOME);
				case '4': /* vt220 End */
				case '8': /* rxvt End */
					return dec_key(n_seen, KEY_END);
				case '3':
					return dec_key(n_seen, KEY_DEL);
				case '5':
					return dec_key(n_seen, KEY_PAGE_UP);
				case '6':
					return dec_key(n_seen, KEY_PAGE_DOWN);
				}
			}
			note(seen, n_seen, b);
			return 033;
		}

		if (b >= 0x20 && b <= 0x3F) {
			/* Parameter or intermediate byte.  Overflow past
			 * ESC_SEEN_MAX keeps consuming toward the final
			 * byte; only the report is truncated. */
			body++;
			note(seen, n_seen, b);
			continue;
		}

		/* Byte outside the CSI grammar: malformed sequence.
		 * Report what was seen, including this byte. */
		note(seen, n_seen, b);
		return 033;
	}
}

int decodeEscapeSequence(escByteSourceFn next, uint8_t *seen, int *n_seen) {
	*n_seen = 0;
	for (;;) {
		uint8_t b;
		/* The byte after ESC: wait indefinitely, ESC is the
		 * Meta prefix.  An abandoned wait (signal) decodes as
		 * an empty unknown sequence. */
		if (next(&b, 1) != 1)
			return 033;
		if (b == 033) {
			continue; /* lone ESC before a new sequence */
		}
		if (b == '[')
			return decodeCSI(next, seen, n_seen);
		if (b == 'O')
			return decodeSS3(next, seen, n_seen);
		if (b >= '0' && b <= '9')
			return KEY_ALT_0 + (b - '0');
		return KEY_META(b);
	}
}
