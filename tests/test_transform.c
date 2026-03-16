/* test_transform.c — Latin Extended case mapping in transform functions. */

#include "test.h"
#include "test_harness.h"
#include "transform.h"
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

/* ---- Upcase ---- */

void test_upcase_ascii(void) {
	uint8_t *r = transformerUpcase((uint8_t *)"hello");
	TEST_ASSERT_EQUAL_STRING("HELLO", (char *)r);
	free(r);
}

void test_upcase_cafe(void) {
	uint8_t *r = transformerUpcase((uint8_t *)"caf\xc3\xa9");
	TEST_ASSERT_EQUAL_STRING("CAF\xc3\x89", (char *)r);
	free(r);
}

void test_upcase_naive(void) {
	/* naïve: n a ï(C3 AF) v e */
	uint8_t *r = transformerUpcase((uint8_t *)"na\xc3\xafve");
	TEST_ASSERT_EQUAL_STRING("NA\xc3\x8fVE", (char *)r);
	free(r);
}

void test_upcase_ano(void) {
	/* año: a ñ(C3 B1) o */
	uint8_t *r = transformerUpcase((uint8_t *)"a\xc3\xb1o");
	TEST_ASSERT_EQUAL_STRING("A\xc3\x91O", (char *)r);
	free(r);
}

void test_upcase_uber(void) {
	/* über: ü(C3 BC) b e r → ÜBER: Ü(C3 9C) B E R */
	uint8_t *r = transformerUpcase((uint8_t *)"\xc3\xbc" "ber");
	TEST_ASSERT_EQUAL_STRING("\xc3\x9c" "BER", (char *)r);
	free(r);
}

void test_upcase_resume(void) {
	/* résumé: r é(C3 A9) s u m é(C3 A9) */
	uint8_t *r = transformerUpcase((uint8_t *)"r\xc3\xa9sum\xc3\xa9");
	TEST_ASSERT_EQUAL_STRING("R\xc3\x89SUM\xc3\x89", (char *)r);
	free(r);
}

void test_upcase_eszett_passthrough(void) {
	/* ß (C3 9F) should pass through unchanged */
	uint8_t *r = transformerUpcase((uint8_t *)"\xc3\x9f");
	TEST_ASSERT_EQUAL_STRING("\xc3\x9f", (char *)r);
	free(r);
}

/* ---- Downcase ---- */

void test_downcase_ascii(void) {
	uint8_t *r = transformerDowncase((uint8_t *)"HELLO");
	TEST_ASSERT_EQUAL_STRING("hello", (char *)r);
	free(r);
}

void test_downcase_cafe(void) {
	uint8_t *r = transformerDowncase((uint8_t *)"CAF\xc3\x89");
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", (char *)r);
	free(r);
}

void test_downcase_uber(void) {
	uint8_t *r = transformerDowncase((uint8_t *)"\xc3\x9c" "BER");
	TEST_ASSERT_EQUAL_STRING("\xc3\xbc" "ber", (char *)r);
	free(r);
}

void test_downcase_ano(void) {
	uint8_t *r = transformerDowncase((uint8_t *)"A\xc3\x91O");
	TEST_ASSERT_EQUAL_STRING("a\xc3\xb1o", (char *)r);
	free(r);
}

/* ---- Capital case ---- */

void test_capital_ascii(void) {
	uint8_t *r = transformerCapitalCase((uint8_t *)"hello world");
	TEST_ASSERT_EQUAL_STRING("Hello World", (char *)r);
	free(r);
}

void test_capital_cafe(void) {
	uint8_t *r = transformerCapitalCase((uint8_t *)"caf\xc3\xa9");
	TEST_ASSERT_EQUAL_STRING("Caf\xc3\xa9", (char *)r);
	free(r);
}

void test_capital_uber(void) {
	/* über → Über: ü at start should become Ü */
	uint8_t *r = transformerCapitalCase((uint8_t *)"\xc3\xbc" "ber");
	TEST_ASSERT_EQUAL_STRING("\xc3\x9c" "ber", (char *)r);
	free(r);
}

void test_capital_resume(void) {
	uint8_t *r = transformerCapitalCase((uint8_t *)"r\xc3\xa9sum\xc3\xa9");
	TEST_ASSERT_EQUAL_STRING("R\xc3\xa9sum\xc3\xa9", (char *)r);
	free(r);
}

void test_capital_mixed_words(void) {
	/* "hello café world" → "Hello Café World" */
	uint8_t *r = transformerCapitalCase(
		(uint8_t *)"hello caf\xc3\xa9 world");
	TEST_ASSERT_EQUAL_STRING("Hello Caf\xc3\xa9 World", (char *)r);
	free(r);
}

void test_capital_ano(void) {
	uint8_t *r = transformerCapitalCase((uint8_t *)"a\xc3\xb1o");
	TEST_ASSERT_EQUAL_STRING("A\xc3\xb1o", (char *)r);
	free(r);
}

void test_capital_leading_accent(void) {
	/* élan → Élan */
	uint8_t *r = transformerCapitalCase((uint8_t *)"\xc3\xa9lan");
	TEST_ASSERT_EQUAL_STRING("\xc3\x89lan", (char *)r);
	free(r);
}

/* ---- Round-trip ---- */

void test_upcase_downcase_roundtrip(void) {
	uint8_t *up = transformerUpcase((uint8_t *)"caf\xc3\xa9");
	uint8_t *down = transformerDowncase(up);
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", (char *)down);
	free(up);
	free(down);
}

/* ---- Latin Extended-A ---- */

void test_upcase_amacron(void) {
	/* ā (C4 81) → Ā (C4 80) */
	uint8_t *r = transformerUpcase((uint8_t *)"\xc4\x81");
	TEST_ASSERT_EQUAL_STRING("\xc4\x80", (char *)r);
	free(r);
}

void test_downcase_amacron(void) {
	/* Ā (C4 80) → ā (C4 81) */
	uint8_t *r = transformerDowncase((uint8_t *)"\xc4\x80");
	TEST_ASSERT_EQUAL_STRING("\xc4\x81", (char *)r);
	free(r);
}

/* ---- Passthrough of non-letter symbols ---- */

void test_upcase_multiply_sign(void) {
	/* × (C3 97 = U+00D7) should pass through unchanged */
	uint8_t *r = transformerUpcase((uint8_t *)"a\xc3\x97" "b");
	TEST_ASSERT_EQUAL_STRING("A\xc3\x97" "B", (char *)r);
	free(r);
}

void test_downcase_divide_sign(void) {
	/* ÷ (C3 B7 = U+00F7) should pass through unchanged */
	uint8_t *r = transformerDowncase((uint8_t *)"A\xc3\xb7" "B");
	TEST_ASSERT_EQUAL_STRING("a\xc3\xb7" "b", (char *)r);
	free(r);
}

void setUp(void) { initTestEditor(); }
void tearDown(void) {}

int main(void) {
	TEST_BEGIN();

	/* Upcase */
	RUN_TEST(test_upcase_ascii);
	RUN_TEST(test_upcase_cafe);
	RUN_TEST(test_upcase_naive);
	RUN_TEST(test_upcase_ano);
	RUN_TEST(test_upcase_uber);
	RUN_TEST(test_upcase_resume);
	RUN_TEST(test_upcase_eszett_passthrough);

	/* Downcase */
	RUN_TEST(test_downcase_ascii);
	RUN_TEST(test_downcase_cafe);
	RUN_TEST(test_downcase_uber);
	RUN_TEST(test_downcase_ano);

	/* Capital case */
	RUN_TEST(test_capital_ascii);
	RUN_TEST(test_capital_cafe);
	RUN_TEST(test_capital_uber);
	RUN_TEST(test_capital_resume);
	RUN_TEST(test_capital_mixed_words);
	RUN_TEST(test_capital_ano);
	RUN_TEST(test_capital_leading_accent);

	/* Round-trip */
	RUN_TEST(test_upcase_downcase_roundtrip);

	/* Latin Extended-A */
	RUN_TEST(test_upcase_amacron);
	RUN_TEST(test_downcase_amacron);

	/* Symbol passthrough */
	RUN_TEST(test_upcase_multiply_sign);
	RUN_TEST(test_downcase_divide_sign);

	return TEST_END();
}
