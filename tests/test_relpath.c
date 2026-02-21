/* test_relpath.c — Unit tests for relativePath(). */

#include "test.h"
#include "test_harness.h"
#include "fileio.h"
#include <stdlib.h>

/* Helper: assert relativePath(from, to) == expected */
#define ASSERT_RELPATH(from, to, expected) do { \
	char *got = relativePath(from, to); \
	if (strcmp(got, expected) != 0) { \
		printf("  FAIL: %s:%d: relativePath(\"%s\", \"%s\")" \
		       " => \"%s\" (expected \"%s\")\n", \
		       __FILE__, __LINE__, from, to, got, expected); \
		_current_test_failed = 1; \
	} \
	free(got); \
} while (0)

/* ---- Same directory ---- */

void test_relpath_same_dir(void) {
	ASSERT_RELPATH("/a/b/c", "/a/b/c", "");
}

void test_relpath_root_to_root(void) {
	ASSERT_RELPATH("/", "/", "");
}

/* ---- Moving up (to is ancestor of from) ---- */

void test_relpath_up_one(void) {
	ASSERT_RELPATH("/a/b/c", "/a/b", "..");
}

void test_relpath_up_two(void) {
	ASSERT_RELPATH("/a/b/c/d", "/a/b", "../..");
}

void test_relpath_up_to_root(void) {
	ASSERT_RELPATH("/a/b", "/", "../..");
}

/* ---- Moving down (to is descendant of from) ---- */

void test_relpath_down_one(void) {
	ASSERT_RELPATH("/a/b", "/a/b/c", "c");
}

void test_relpath_down_two(void) {
	ASSERT_RELPATH("/a/b", "/a/b/c/d", "c/d");
}

void test_relpath_down_from_root(void) {
	ASSERT_RELPATH("/", "/a/b", "a/b");
}

/* ---- Sibling directories ---- */

void test_relpath_sibling(void) {
	ASSERT_RELPATH("/a/b/c", "/a/b/d", "../d");
}

void test_relpath_sibling_at_root(void) {
	ASSERT_RELPATH("/a", "/b", "../b");
}

/* ---- Divergent paths ---- */

void test_relpath_divergent(void) {
	ASSERT_RELPATH("/a/b/c", "/a/d", "../../d");
}

void test_relpath_completely_different(void) {
	ASSERT_RELPATH("/backup/opt/project", "/opt/project",
		       "../../../opt/project");
}

/* ---- Deeper divergence ---- */

void test_relpath_deep_divergence(void) {
	ASSERT_RELPATH("/a/b/c", "/x/y/z", "../../../x/y/z");
}

/* ---- Partial name overlap (not a real prefix) ---- */

void test_relpath_partial_name_overlap(void) {
	/* /a/bar and /a/baz share "/a/ba" in characters but
	 * the common directory is /a, not /a/ba */
	ASSERT_RELPATH("/a/bar", "/a/baz", "../baz");
}

void test_relpath_prefix_in_name(void) {
	/* /abc is not a prefix of /abcdef at directory level */
	ASSERT_RELPATH("/abc", "/abcdef", "../abcdef");
}

/* ---- Real-world cd scenario ---- */

void test_relpath_cd_into_subdir(void) {
	ASSERT_RELPATH("/home/user/project/src", "/home/user/project", "..");
}

void test_relpath_cd_into_deep_subdir(void) {
	ASSERT_RELPATH("/home/user/project/src/lib", "/home/user/project",
		       "../..");
}

void test_relpath_cd_up_from_subdir(void) {
	ASSERT_RELPATH("/home/user/project", "/home/user/project/src", "src");
}

void test_relpath_cd_to_sibling_project(void) {
	ASSERT_RELPATH("/home/user/project-b", "/home/user/project-a",
		       "../project-a");
}

/* Helper: assert rebaseFilename(fn, old, new) == expected */
#define ASSERT_REBASE(fn, old_cwd, new_cwd, expected) do { \
	char *got = rebaseFilename(fn, old_cwd, new_cwd); \
	if (strcmp(got, expected) != 0) { \
		printf("  FAIL: %s:%d: rebaseFilename(\"%s\", \"%s\", \"%s\")" \
		       " => \"%s\" (expected \"%s\")\n", \
		       __FILE__, __LINE__, fn, old_cwd, new_cwd, \
		       got, expected); \
		_current_test_failed = 1; \
	} \
	free(got); \
} while (0)

/* ---- rebaseFilename: basic cases ---- */

void test_rebase_same_dir(void) {
	ASSERT_REBASE("README.md", "/home/user", "/home/user", "README.md");
}

void test_rebase_cd_to_parent(void) {
	ASSERT_REBASE("README.md", "/home/user", "/home", "user/README.md");
}

void test_rebase_cd_to_child(void) {
	ASSERT_REBASE("README.md", "/home/user", "/home/user/src",
		      "../README.md");
}

void test_rebase_cd_to_sibling(void) {
	ASSERT_REBASE("README.md", "/home/user", "/home/other",
		      "../user/README.md");
}

void test_rebase_absolute_unchanged(void) {
	ASSERT_REBASE("/etc/config", "/home/user", "/tmp", "/etc/config");
}

/* ---- rebaseFilename: subdir filenames ---- */

void test_rebase_subdir_file_cd_parent(void) {
	ASSERT_REBASE("src/main.c", "/home/user", "/home",
		      "user/src/main.c");
}

void test_rebase_subdir_file_cd_child(void) {
	ASSERT_REBASE("src/main.c", "/home/user", "/home/user/src",
		      "main.c");
}

void test_rebase_subdir_file_cd_sibling_dir(void) {
	ASSERT_REBASE("src/main.c", "/home/user", "/home/user/docs",
		      "../src/main.c");
}

/* ---- rebaseFilename: the double-cd bug scenario ---- */

void test_rebase_double_cd(void) {
	/* Start in /opt/project, file is README.md
	 * cd .. => /opt, file becomes project/README.md */
	char *after_first = rebaseFilename("README.md", "/opt/project", "/opt");
	TEST_ASSERT_EQUAL_STRING("project/README.md", after_first);

	/* Now cd project/tests => /opt/project/tests
	 * file should become ../README.md */
	char *after_second = rebaseFilename(after_first, "/opt",
					    "/opt/project/tests");
	TEST_ASSERT_EQUAL_STRING("../README.md", after_second);

	free(after_first);
	free(after_second);
}

void test_rebase_triple_cd(void) {
	/* /a/b/c, file=foo.txt => cd /a => b/c/foo.txt */
	char *r1 = rebaseFilename("foo.txt", "/a/b/c", "/a");
	TEST_ASSERT_EQUAL_STRING("b/c/foo.txt", r1);

	/* cd /a/b => c/foo.txt */
	char *r2 = rebaseFilename(r1, "/a", "/a/b");
	TEST_ASSERT_EQUAL_STRING("c/foo.txt", r2);

	/* cd /x => ../a/b/c/foo.txt */
	char *r3 = rebaseFilename(r2, "/a/b", "/x");
	TEST_ASSERT_EQUAL_STRING("../a/b/c/foo.txt", r3);

	free(r1);
	free(r2);
	free(r3);
}

/* ---- The bug: filename already contains .. ---- */

void test_rebase_dotdot_in_filename(void) {
	/* In /opt/project/tests, file is ../README.md
	 * cd to /opt => should be project/README.md */
	ASSERT_REBASE("../README.md", "/opt/project/tests", "/opt",
		      "project/README.md");
}

void test_rebase_dotdot_cd_up_two(void) {
	/* In /opt/project/tests, file is ../README.md
	 * cd to /opt/project => should be README.md */
	ASSERT_REBASE("../README.md", "/opt/project/tests", "/opt/project",
		      "README.md");
}

void test_rebase_double_cd_with_dotdot(void) {
	/* The exact reported bug:
	 * Start in /opt/project, file is README.md
	 * cd to /opt/project/tests => ../README.md */
	char *r1 = rebaseFilename("README.md", "/opt/project",
				  "/opt/project/tests");
	TEST_ASSERT_EQUAL_STRING("../README.md", r1);

	/* cd to /opt => should be project/README.md, NOT
	 * project/tests/../README.md */
	char *r2 = rebaseFilename(r1, "/opt/project/tests", "/opt");
	TEST_ASSERT_EQUAL_STRING("project/README.md", r2);

	free(r1);
	free(r2);
}
void setUp(void) { initTestEditor(); }
void tearDown(void) {}

int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_relpath_same_dir);
	RUN_TEST(test_relpath_root_to_root);

	RUN_TEST(test_relpath_up_one);
	RUN_TEST(test_relpath_up_two);
	RUN_TEST(test_relpath_up_to_root);

	RUN_TEST(test_relpath_down_one);
	RUN_TEST(test_relpath_down_two);
	RUN_TEST(test_relpath_down_from_root);

	RUN_TEST(test_relpath_sibling);
	RUN_TEST(test_relpath_sibling_at_root);

	RUN_TEST(test_relpath_divergent);
	RUN_TEST(test_relpath_completely_different);
	RUN_TEST(test_relpath_deep_divergence);

	RUN_TEST(test_relpath_partial_name_overlap);
	RUN_TEST(test_relpath_prefix_in_name);

	RUN_TEST(test_relpath_cd_into_subdir);
	RUN_TEST(test_relpath_cd_into_deep_subdir);
	RUN_TEST(test_relpath_cd_up_from_subdir);
	RUN_TEST(test_relpath_cd_to_sibling_project);

	RUN_TEST(test_rebase_same_dir);
	RUN_TEST(test_rebase_cd_to_parent);
	RUN_TEST(test_rebase_cd_to_child);
	RUN_TEST(test_rebase_cd_to_sibling);
	RUN_TEST(test_rebase_absolute_unchanged);
	RUN_TEST(test_rebase_subdir_file_cd_parent);
	RUN_TEST(test_rebase_subdir_file_cd_child);
	RUN_TEST(test_rebase_subdir_file_cd_sibling_dir);
	RUN_TEST(test_rebase_double_cd);
	RUN_TEST(test_rebase_triple_cd);
	RUN_TEST(test_rebase_dotdot_in_filename);
	RUN_TEST(test_rebase_dotdot_cd_up_two);
	RUN_TEST(test_rebase_double_cd_with_dotdot);

	return TEST_END();
}
