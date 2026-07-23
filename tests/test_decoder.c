/* test_decoder.c: unit tests for the escape-sequence decoder.
 *
 * The decoder is a pure state machine over a byte source
 * (decoder.h), so these tests drive it entirely from byte arrays:
 * every mapped key, both timeout classes, unknown and malformed
 * sequences, the lone-ESC-then-sequence rule, overflow, and a
 * deterministic random-stream soak.  No terminal is involved;
 * terminal-level behavior (real timing, signals, rendering) is
 * covered by tests/decoder_pty_test.py.
 */

#include "test.h"
#include "test_harness.h"
#include "decoder.h"
#include "keymap.h"
#include <string.h>

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* Scripted byte source.  script/script_len hold the bytes "after the
 * ESC"; exhaustion models a timeout (returns 0), which is also what
 * an abandoned indefinite wait returns. */
static const uint8_t *script;
static int script_len;
static int script_pos;

static int scriptSource(uint8_t *out, int wait_indefinitely) {
	(void)wait_indefinitely;
	if (script_pos >= script_len)
		return 0;
	*out = script[script_pos++];
	return 1;
}

/* Decode one sequence from the given bytes; returns the token and
 * exposes the report via out_seen/out_n (may be NULL). */
static int decodeBytes(const uint8_t *bytes, int len, uint8_t *out_seen,
		       int *out_n) {
	uint8_t seen[ESC_SEEN_MAX];
	int n_seen = 0;
	script = bytes;
	script_len = len;
	script_pos = 0;
	int key = decodeEscapeSequence(scriptSource, seen, &n_seen);
	if (out_seen)
		memcpy(out_seen, seen, sizeof(seen));
	if (out_n)
		*out_n = n_seen;
	return key;
}

static int decodeStr(const char *s) {
	return decodeBytes((const uint8_t *)s, (int)strlen(s), NULL, NULL);
}

/* ---- Mapped keys: the frozen contract ---- */

void test_csi_letter_finals(void) {
	TEST_ASSERT_EQUAL_INT(KEY_ARROW_UP, decodeStr("[A"));
	TEST_ASSERT_EQUAL_INT(KEY_ARROW_DOWN, decodeStr("[B"));
	TEST_ASSERT_EQUAL_INT(KEY_ARROW_RIGHT, decodeStr("[C"));
	TEST_ASSERT_EQUAL_INT(KEY_ARROW_LEFT, decodeStr("[D"));
	TEST_ASSERT_EQUAL_INT(KEY_END, decodeStr("[F"));
	TEST_ASSERT_EQUAL_INT(KEY_HOME, decodeStr("[H"));
	TEST_ASSERT_EQUAL_INT(KEY_BACKTAB, decodeStr("[Z"));
}

void test_csi_tilde_keys(void) {
	TEST_ASSERT_EQUAL_INT(KEY_HOME, decodeStr("[1~"));
	TEST_ASSERT_EQUAL_INT(KEY_DEL, decodeStr("[3~"));
	TEST_ASSERT_EQUAL_INT(KEY_END, decodeStr("[4~"));
	TEST_ASSERT_EQUAL_INT(KEY_PAGE_UP, decodeStr("[5~"));
	TEST_ASSERT_EQUAL_INT(KEY_PAGE_DOWN, decodeStr("[6~"));
	TEST_ASSERT_EQUAL_INT(KEY_HOME, decodeStr("[7~"));
	TEST_ASSERT_EQUAL_INT(KEY_END, decodeStr("[8~"));
}

void test_ss3_keys(void) {
	TEST_ASSERT_EQUAL_INT(KEY_ARROW_UP, decodeStr("OA"));
	TEST_ASSERT_EQUAL_INT(KEY_ARROW_DOWN, decodeStr("OB"));
	TEST_ASSERT_EQUAL_INT(KEY_ARROW_RIGHT, decodeStr("OC"));
	TEST_ASSERT_EQUAL_INT(KEY_ARROW_LEFT, decodeStr("OD"));
	TEST_ASSERT_EQUAL_INT(KEY_HOME, decodeStr("OH"));
	TEST_ASSERT_EQUAL_INT(KEY_END, decodeStr("OF"));
}

void test_alt_digits_and_meta(void) {
	TEST_ASSERT_EQUAL_INT(KEY_ALT_0, decodeStr("0"));
	TEST_ASSERT_EQUAL_INT(KEY_ALT_0 + 7, decodeStr("7"));
	TEST_ASSERT_EQUAL_INT(KEY_META('f'), decodeStr("f"));
	TEST_ASSERT_EQUAL_INT(KEY_META('<'), decodeStr("<"));
}

/* ---- Timeout classes ---- */

void test_alt_bracket_and_alt_shift_o(void) {
	/* Nothing follows the introducer: Alt+[ / Alt+Shift+O. */
	TEST_ASSERT_EQUAL_INT(KEY_META('['), decodeStr("["));
	TEST_ASSERT_EQUAL_INT(KEY_META('O'), decodeStr("O"));
}

void test_abandoned_meta_wait(void) {
	/* Empty source: the wait for the byte after ESC was
	 * abandoned (signal).  Decodes as an empty unknown. */
	int n = -1;
	int key = decodeBytes(NULL, 0, NULL, &n);
	TEST_ASSERT_EQUAL_INT(033, key);
	TEST_ASSERT_EQUAL_INT(0, n);
}

void test_incomplete_csi_reports_bytes(void) {
	uint8_t seen[ESC_SEEN_MAX];
	int n;
	int key = decodeBytes((const uint8_t *)"[1;5", 4, seen, &n);
	TEST_ASSERT_EQUAL_INT(033, key);
	TEST_ASSERT_EQUAL_INT(4, n);
	TEST_ASSERT(memcmp(seen, "[1;5", 4) == 0);
}

/* ---- Unknown and malformed sequences ---- */

void test_unknown_sequences_report_and_consume(void) {
	uint8_t seen[ESC_SEEN_MAX];
	int n;

	/* F5: multi-digit tilde code. */
	int key = decodeBytes((const uint8_t *)"[15~", 4, seen, &n);
	TEST_ASSERT_EQUAL_INT(033, key);
	TEST_ASSERT_EQUAL_INT(4, n);
	TEST_ASSERT(memcmp(seen, "[15~", 4) == 0);

	/* Ctrl-Right: modified arrow.  Fully consumed. */
	key = decodeBytes((const uint8_t *)"[1;5C", 5, seen, &n);
	TEST_ASSERT_EQUAL_INT(033, key);
	TEST_ASSERT_EQUAL_INT(5, n);
	TEST_ASSERT_EQUAL_INT(5, script_pos); /* nothing left queued */

	/* SS3 F1. */
	key = decodeBytes((const uint8_t *)"OP", 2, seen, &n);
	TEST_ASSERT_EQUAL_INT(033, key);
	TEST_ASSERT_EQUAL_INT(2, n);
	TEST_ASSERT(seen[0] == 'O' && seen[1] == 'P');
}

void test_malformed_csi_stops_at_bad_byte(void) {
	uint8_t seen[ESC_SEEN_MAX];
	int n;
	/* 033 inside a CSI body is outside the grammar. */
	const uint8_t bytes[] = { '[', '1', 033, 'x' };
	int key = decodeBytes(bytes, 4, seen, &n);
	TEST_ASSERT_EQUAL_INT(033, key);
	TEST_ASSERT_EQUAL_INT(3, n); /* "[1" + the offending byte */
	TEST_ASSERT_EQUAL_INT(3, script_pos); /* 'x' left for the user */
}

void test_recognized_keys_report_nothing(void) {
	uint8_t seen[ESC_SEEN_MAX];
	int n = 99;
	int key = decodeBytes((const uint8_t *)"[5~", 3, seen, &n);
	TEST_ASSERT_EQUAL_INT(KEY_PAGE_UP, key);
	TEST_ASSERT_EQUAL_INT(0, n);
}

/* ---- The lone-ESC rule ---- */

void test_lone_esc_before_sequence_is_discarded(void) {
	/* ESC pressed, then Up arrow: the arrow must decode; its
	 * body must not leak.  (The leading ESC of the arrow's own
	 * sequence appears as a repeated introducer.) */
	const uint8_t bytes[] = { 033, '[', 'A' };
	TEST_ASSERT_EQUAL_INT(KEY_ARROW_UP, decodeBytes(bytes, 3, NULL, NULL));

	/* Several lone ESCs, then Meta-f. */
	const uint8_t bytes2[] = { 033, 033, 033, 'f' };
	TEST_ASSERT_EQUAL_INT(KEY_META('f'),
			      decodeBytes(bytes2, 4, NULL, NULL));
}

/* ---- Overflow ---- */

void test_long_csi_consumed_report_truncated(void) {
	/* 30 parameter bytes then a final: the report truncates at
	 * ESC_SEEN_MAX, but every byte is consumed. */
	uint8_t bytes[32];
	bytes[0] = '[';
	for (int i = 1; i <= 30; i++)
		bytes[i] = '0' + (i % 10);
	bytes[31] = 'u';
	uint8_t seen[ESC_SEEN_MAX];
	int n;
	int key = decodeBytes(bytes, 32, seen, &n);
	TEST_ASSERT_EQUAL_INT(033, key);
	TEST_ASSERT_EQUAL_INT(ESC_SEEN_MAX, n);
	TEST_ASSERT_EQUAL_INT(32, script_pos); /* fully drained */
}

/* ---- Deterministic random soak ----
 *
 * Feed the machine random byte streams and assert the safety
 * properties that must hold for ANY input: it terminates, it never
 * reads past the stream, and the report length is bounded.  This is
 * the cheap standing fuzz target the pure interface exists for. */

static uint32_t rng_state = 0x2050u;
static uint32_t rngNext(void) {
	/* xorshift32: deterministic across platforms. */
	uint32_t x = rng_state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	rng_state = x;
	return x;
}

void test_random_stream_soak(void) {
	uint8_t bytes[24];
	uint8_t seen[ESC_SEEN_MAX];
	for (int iter = 0; iter < 20000; iter++) {
		int len = (int)(rngNext() % (sizeof(bytes) + 1));
		for (int i = 0; i < len; i++)
			bytes[i] = (uint8_t)rngNext();
		int n = -1;
		int key = decodeBytes(bytes, len, seen, &n);
		TEST_ASSERT(script_pos <= len);
		TEST_ASSERT(n >= 0 && n <= ESC_SEEN_MAX);
		TEST_ASSERT(key == 033 || n == 0);
	}
}

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_csi_letter_finals);
	RUN_TEST(test_csi_tilde_keys);
	RUN_TEST(test_ss3_keys);
	RUN_TEST(test_alt_digits_and_meta);
	RUN_TEST(test_alt_bracket_and_alt_shift_o);
	RUN_TEST(test_abandoned_meta_wait);
	RUN_TEST(test_incomplete_csi_reports_bytes);
	RUN_TEST(test_unknown_sequences_report_and_consume);
	RUN_TEST(test_malformed_csi_stops_at_bad_byte);
	RUN_TEST(test_recognized_keys_report_nothing);
	RUN_TEST(test_lone_esc_before_sequence_is_discarded);
	RUN_TEST(test_long_csi_consumed_report_truncated);
	RUN_TEST(test_random_stream_soak);
	return TEST_END();
}
