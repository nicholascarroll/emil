/* test_tilde.c — Tests for expandTilde(), collapseHome(), absolutePath().
 *
 * These are pure string functions.  We control $HOME via setenv()
 * so the tests are deterministic regardless of the real user. */

#include "test.h"
#include "test_harness.h"
#include "util.h"
#include <stdlib.h>
#include <string.h>

void setUp(void) {
	initTestEditor();
	setenv("HOME", "/home/testuser", 1);
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- expandTilde ---- */

void test_expand_tilde_slash(void) {
	char *r = expandTilde("~/Documents");
	TEST_ASSERT_EQUAL_STRING("/home/testuser/Documents", r);
	free(r);
}

void test_expand_tilde_alone(void) {
	char *r = expandTilde("~");
	TEST_ASSERT_EQUAL_STRING("/home/testuser", r);
	free(r);
}

void test_expand_tilde_slash_only(void) {
	char *r = expandTilde("~/");
	TEST_ASSERT_EQUAL_STRING("/home/testuser/", r);
	free(r);
}

void test_expand_tilde_nested(void) {
	char *r = expandTilde("~/a/b/c.txt");
	TEST_ASSERT_EQUAL_STRING("/home/testuser/a/b/c.txt", r);
	free(r);
}

void test_expand_no_tilde(void) {
	char *r = expandTilde("/usr/bin/foo");
	TEST_ASSERT_EQUAL_STRING("/usr/bin/foo", r);
	free(r);
}

void test_expand_relative(void) {
	char *r = expandTilde("src/main.c");
	TEST_ASSERT_EQUAL_STRING("src/main.c", r);
	free(r);
}

void test_expand_tilde_otheruser(void) {
	char *r = expandTilde("~otheruser/file");
	TEST_ASSERT_EQUAL_STRING("~otheruser/file", r);
	free(r);
}

void test_expand_tilde_no_home(void) {
	unsetenv("HOME");
	char *r = expandTilde("~/foo");
	TEST_ASSERT_EQUAL_STRING("~/foo", r);
	free(r);
	setenv("HOME", "/home/testuser", 1);
}

/* ---- collapseHome ---- */

void test_collapse_basic(void) {
	char *r = collapseHome("/home/testuser/Documents");
	TEST_ASSERT_EQUAL_STRING("~/Documents", r);
	free(r);
}

void test_collapse_exact_home(void) {
	char *r = collapseHome("/home/testuser");
	TEST_ASSERT_EQUAL_STRING("~", r);
	free(r);
}

void test_collapse_home_slash(void) {
	char *r = collapseHome("/home/testuser/");
	TEST_ASSERT_EQUAL_STRING("~/", r);
	free(r);
}

void test_collapse_nested(void) {
	char *r = collapseHome("/home/testuser/a/b/c.txt");
	TEST_ASSERT_EQUAL_STRING("~/a/b/c.txt", r);
	free(r);
}

void test_collapse_not_under_home(void) {
	char *r = collapseHome("/usr/local/bin");
	TEST_ASSERT_EQUAL_STRING("/usr/local/bin", r);
	free(r);
}

void test_collapse_prefix_false_match(void) {
	char *r = collapseHome("/home/testusername/file");
	TEST_ASSERT_EQUAL_STRING("/home/testusername/file", r);
	free(r);
}

void test_collapse_relative(void) {
	char *r = collapseHome("src/main.c");
	TEST_ASSERT_EQUAL_STRING("src/main.c", r);
	free(r);
}

void test_collapse_no_home(void) {
	unsetenv("HOME");
	char *r = collapseHome("/home/testuser/foo");
	TEST_ASSERT_EQUAL_STRING("/home/testuser/foo", r);
	free(r);
	setenv("HOME", "/home/testuser", 1);
}

void test_collapse_special_buffer(void) {
	char *r = collapseHome("*scratch*");
	TEST_ASSERT_EQUAL_STRING("*scratch*", r);
	free(r);
}

/* ---- round-trip ---- */

void test_roundtrip_expand_then_collapse(void) {
	char *expanded = expandTilde("~/projects/foo.c");
	char *collapsed = collapseHome(expanded);
	TEST_ASSERT_EQUAL_STRING("~/projects/foo.c", collapsed);
	free(expanded);
	free(collapsed);
}

void test_roundtrip_absolute_collapse(void) {
	char *collapsed = collapseHome("/home/testuser/bar.c");
	char *expanded = expandTilde(collapsed);
	TEST_ASSERT_EQUAL_STRING("/home/testuser/bar.c", expanded);
	free(collapsed);
	free(expanded);
}

/* ---- trailing slash on HOME ---- */

void test_collapse_home_trailing_slash(void) {
	setenv("HOME", "/home/testuser/", 1);
	char *r = collapseHome("/home/testuser/file.c");
	TEST_ASSERT_EQUAL_STRING("~/file.c", r);
	free(r);
	setenv("HOME", "/home/testuser", 1);
}

/* ---- absolutePath ---- */

void test_abspath_already_absolute(void) {
	char *r = absolutePath("/usr/bin/foo");
	TEST_ASSERT_EQUAL_STRING("/usr/bin/foo", r);
	free(r);
}

void test_abspath_tilde(void) {
	char *r = absolutePath("~/file.c");
	TEST_ASSERT_EQUAL_STRING("/home/testuser/file.c", r);
	free(r);
}

void test_abspath_tilde_alone(void) {
	char *r = absolutePath("~");
	TEST_ASSERT_EQUAL_STRING("/home/testuser", r);
	free(r);
}

void test_abspath_relative(void) {
	char *r = absolutePath("src/main.c");
	TEST_ASSERT(r[0] == '/');
	int rlen = strlen(r);
	TEST_ASSERT(rlen >= 10);
	TEST_ASSERT_EQUAL_STRING("src/main.c", r + rlen - 10);
	free(r);
}

void test_abspath_normalizes_dotdot(void) {
	char *r = absolutePath("/home/me/subdir/../foo.c");
	TEST_ASSERT_EQUAL_STRING("/home/me/foo.c", r);
	free(r);
}

void test_abspath_normalizes_dot(void) {
	char *r = absolutePath("/home/me/./foo.c");
	TEST_ASSERT_EQUAL_STRING("/home/me/foo.c", r);
	free(r);
}

void test_abspath_empty(void) {
	char *r = absolutePath("");
	TEST_ASSERT_EQUAL_STRING("", r);
	free(r);
}

int main(void) {
	TEST_BEGIN();

	/* expandTilde */
	RUN_TEST(test_expand_tilde_slash);
	RUN_TEST(test_expand_tilde_alone);
	RUN_TEST(test_expand_tilde_slash_only);
	RUN_TEST(test_expand_tilde_nested);
	RUN_TEST(test_expand_no_tilde);
	RUN_TEST(test_expand_relative);
	RUN_TEST(test_expand_tilde_otheruser);
	RUN_TEST(test_expand_tilde_no_home);

	/* collapseHome */
	RUN_TEST(test_collapse_basic);
	RUN_TEST(test_collapse_exact_home);
	RUN_TEST(test_collapse_home_slash);
	RUN_TEST(test_collapse_nested);
	RUN_TEST(test_collapse_not_under_home);
	RUN_TEST(test_collapse_prefix_false_match);
	RUN_TEST(test_collapse_relative);
	RUN_TEST(test_collapse_no_home);
	RUN_TEST(test_collapse_special_buffer);

	/* round-trip */
	RUN_TEST(test_roundtrip_expand_then_collapse);
	RUN_TEST(test_roundtrip_absolute_collapse);

	/* trailing slash */
	RUN_TEST(test_collapse_home_trailing_slash);

	/* absolutePath */
	RUN_TEST(test_abspath_already_absolute);
	RUN_TEST(test_abspath_tilde);
	RUN_TEST(test_abspath_tilde_alone);
	RUN_TEST(test_abspath_relative);
	RUN_TEST(test_abspath_normalizes_dotdot);
	RUN_TEST(test_abspath_normalizes_dot);
	RUN_TEST(test_abspath_empty);

	return TEST_END();
}
