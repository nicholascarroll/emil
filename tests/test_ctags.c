/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_ctags.c: Tests for ctags tags-file discovery and path
 * resolution.
 */

#include "test.h"
#include "test_harness.h"
#include "ctags.h"
#include "fileio.h"
#include "keymap.h"
#include "util.h" /* emil_strlcpy prototype — without it the call at
		    * make_project() is an implicit declaration (assumed
		    * int return), which is invalid C99 and trips
		    * -Wsign-compare on the size_t comparison. */
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- resolveTagPath: pure string join ---- */

void test_resolve_relative_joins_onto_tagsdir(void) {
	char out[PATH_MAX];
	int rc = resolveTagPath("/proj", "src/foo.c", out, sizeof(out));
	TEST_ASSERT_EQUAL_INT(0, rc);
	TEST_ASSERT_EQUAL_STRING("/proj/src/foo.c", out);
}

void test_resolve_bare_filename(void) {
	char out[PATH_MAX];
	resolveTagPath("/proj", "foo.c", out, sizeof(out));
	TEST_ASSERT_EQUAL_STRING("/proj/foo.c", out);
}

void test_resolve_absolute_passes_through(void) {
	/* An absolute tag path must NOT be joined onto tagsdir. */
	char out[PATH_MAX];
	resolveTagPath("/proj", "/usr/include/stdio.h", out, sizeof(out));
	TEST_ASSERT_EQUAL_STRING("/usr/include/stdio.h", out);
}

void test_resolve_home_passes_through(void) {
	char out[PATH_MAX];
	resolveTagPath("/proj", "~/foo.c", out, sizeof(out));
	TEST_ASSERT_EQUAL_STRING("~/foo.c", out);
}

void test_resolve_root_tagsdir(void) {
	char out[PATH_MAX];
	resolveTagPath("/", "src/foo.c", out, sizeof(out));
	/* Joining onto "/" yields a leading double slash; that's a
	 * benign path form (POSIX treats "//src" as "/src") and is what
	 * the production join produces, so lock it in rather than
	 * special-casing. */
	TEST_ASSERT_EQUAL_STRING("//src/foo.c", out);
}

void test_resolve_truncation_reported(void) {
	char out[16];
	int rc = resolveTagPath("/a/very/long/directory/name",
				"deep/nested/path/file.c", out, sizeof(out));
	TEST_ASSERT_EQUAL_INT(-1, rc);
}

void test_resolve_absolute_truncation_reported(void) {
	char out[8];
	int rc = resolveTagPath("/x", "/usr/include/stdio.h", out,
				sizeof(out));
	TEST_ASSERT_EQUAL_INT(-1, rc);
}

/* ---- findTagsDir: upward search on a real temp tree ---- */

/* Build a temp project:  <root>/tags  and <root>/src/deep/  */
static int make_project(char *root, size_t rootsz) {
	char tmpl[] = "/tmp/emil_ctags_XXXXXX";
	char *made = mkdtemp(tmpl);
	if (!made)
		return -1;
	if (emil_strlcpy(root, made, rootsz) >= rootsz)
		return -1;

	char path[PATH_MAX];
	snprintf(path, sizeof(path), "%s/tags", root);
	FILE *fp = fopen(path, "w");
	if (!fp)
		return -1;
	fputs("helper_fn\tsrc/foo.c\t/^int helper_fn(void)$/;\"\tf\n", fp);
	fclose(fp);

	snprintf(path, sizeof(path), "%s/src", root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/src/deep", root);
	mkdir(path, 0755);
	return 0;
}

static void rm_project(const char *root) {
	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
	int r = system(cmd);
	(void)r;
}

void test_find_tags_from_root(void) {
	char root[PATH_MAX];
	if (make_project(root, sizeof(root)) != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	char saved[PATH_MAX];
	TEST_ASSERT_NOT_NULL(getcwd(saved, sizeof(saved)));

	TEST_ASSERT_EQUAL_INT(0, chdir(root));
	char found[PATH_MAX];
	int rc = findTagsDir(found, sizeof(found));
	TEST_ASSERT_EQUAL_INT(0, rc);
	/* Resolve both sides through a real path to dodge /tmp ->
	 * /private/tmp style symlinks on some platforms. */
	char rp_found[PATH_MAX], rp_root[PATH_MAX];
	TEST_ASSERT_NOT_NULL(realpath(found, rp_found));
	TEST_ASSERT_NOT_NULL(realpath(root, rp_root));
	TEST_ASSERT_EQUAL_STRING(rp_root, rp_found);

	TEST_ASSERT_EQUAL_INT(0, chdir(saved));
	rm_project(root);
}

void test_find_tags_from_subdir(void) {
	/* The case that used to fail entirely. */
	char root[PATH_MAX];
	if (make_project(root, sizeof(root)) != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	char saved[PATH_MAX];
	TEST_ASSERT_NOT_NULL(getcwd(saved, sizeof(saved)));

	/* Sized to hold the longest suffix appended below, so the
	 * compiler can see the result always fits. */
	char sub[PATH_MAX + 16];
	snprintf(sub, sizeof(sub), "%s/src/deep", root);
	TEST_ASSERT_EQUAL_INT(0, chdir(sub));

	char found[PATH_MAX];
	int rc = findTagsDir(found, sizeof(found));
	TEST_ASSERT_EQUAL_INT(0, rc);
	char rp_found[PATH_MAX], rp_root[PATH_MAX];
	TEST_ASSERT_NOT_NULL(realpath(found, rp_found));
	TEST_ASSERT_NOT_NULL(realpath(root, rp_root));
	TEST_ASSERT_EQUAL_STRING(rp_root, rp_found);

	/* End-to-end: resolve the tag's stored path against the found
	 * directory and confirm it points at the right absolute file. */
	char resolved[PATH_MAX];
	resolveTagPath(found, "src/foo.c", resolved, sizeof(resolved));
	char expect[PATH_MAX + 16];
	snprintf(expect, sizeof(expect), "%s/src/foo.c", found);
	TEST_ASSERT_EQUAL_STRING(expect, resolved);

	TEST_ASSERT_EQUAL_INT(0, chdir(saved));
	rm_project(root);
}

void test_find_tags_absent(void) {
	/* A directory tree with no tags file anywhere up to root must
	 * report failure, not succeed with garbage. */
	char tmpl[] = "/tmp/emil_notags_XXXXXX";
	char *dir = mkdtemp(tmpl);
	TEST_ASSERT_NOT_NULL(dir);
	char saved[PATH_MAX];
	TEST_ASSERT_NOT_NULL(getcwd(saved, sizeof(saved)));
	TEST_ASSERT_EQUAL_INT(0, chdir(dir));

	/* NOTE: this assumes no "tags" file exists in any ancestor of a
	 * fresh /tmp/xxx directory, which holds on normal systems. */
	char found[PATH_MAX];
	int rc = findTagsDir(found, sizeof(found));
	TEST_ASSERT_EQUAL_INT(-1, rc);

	TEST_ASSERT_EQUAL_INT(0, chdir(saved));
	rm_project(dir);
}

/* ---- ctagsHyphenatedWordAtPoint: Lisp-style symbols ---- */

static char *hyphenated_at(const char *line, int cx) {
	struct buffer *buf = make_test_buffer(line);
	buf->cx = cx;
	return ctagsHyphenatedWordAtPoint();
}

/* Called several times per test, so it frees its own buffer: the
 * harness only cleans up the last one. */
static void assert_hyphenated(const char *line, int cx, const char *want) {
	char *w = hyphenated_at(line, cx);
	if (want)
		TEST_ASSERT_EQUAL_STRING(want, w);
	else
		TEST_ASSERT_TRUE(w == NULL);
	free(w);
	destroyBuffer(E.buf);
	E.buf = NULL;
	E.headbuf = NULL;
	E.windows[0]->buf = NULL;
}

void test_hyphenated_several_single_hyphens(void) {
	assert_hyphenated("(foo-bar-baz x)", 1, "foo-bar-baz");
	assert_hyphenated("(foo-bar-baz x)", 6, "foo-bar-baz");
	assert_hyphenated("(foo-bar-baz x)", 10, "foo-bar-baz");
}

/* Emacs Lisp names internal functions pkg--name. */
void test_hyphenated_double_hyphen(void) {
	assert_hyphenated("(my-pkg--helper x)", 1, "my-pkg--helper");
	assert_hyphenated("(my-pkg--helper x)", 5, "my-pkg--helper");
	assert_hyphenated("(my-pkg--helper x)", 10, "my-pkg--helper");
}

/* Point on the first hyphen, just after a word. */
void test_hyphenated_on_double_hyphen(void) {
	assert_hyphenated("(my-pkg--helper x)", 7, "my-pkg--helper");
}

/* Hyphens not followed by a word stay out of the symbol. */
void test_hyphenated_trailing_hyphens_excluded(void) {
	assert_hyphenated("foo-bar-- x", 1, "foo-bar");
	assert_hyphenated("x --foo-bar", 4, "foo-bar");
}

/* C: a - b and x--y are not symbols. Plain word, so NULL. */
void test_hyphenated_spaced_minus_is_not_symbol(void) {
	assert_hyphenated("a - b", 0, NULL);
}

/* ---- ctagsWordAtPoint: ASCII identifiers and non-ASCII words ---- */

/* "ภาษา" ZWSP "ไทย" ZWSP "ง่าย" -- three words, 12 + 3 + 9 + 3 + 12
 * bytes.  Offsets below are byte offsets into this row. */
#define ZW "\xe2\x80\x8b"
static const char *thai_row = "ภาษา" ZW "ไทย" ZW "ง่าย";

static char *word_at(const char *line, int cx) {
	struct buffer *buf = make_test_buffer(line);
	buf->cx = cx;
	return ctagsWordAtPoint();
}

void test_word_ascii_identifier_unchanged(void) {
	char *w = word_at("foo_bar(x)", 2);
	TEST_ASSERT_EQUAL_STRING("foo_bar", w);
	free(w);
}

void test_word_ascii_just_after_identifier(void) {
	char *w = word_at("foo_bar(x)", 7);
	TEST_ASSERT_EQUAL_STRING("foo_bar", w);
	free(w);
}

void test_word_none_on_punctuation(void) {
	struct buffer *buf = make_test_buffer("a + b");
	buf->cx = 2;
	char *w = ctagsWordAtPoint();
	TEST_ASSERT_NULL(w);
}

void test_word_thai_first_word(void) {
	char *w = word_at(thai_row, 3); /* on the second codepoint */
	TEST_ASSERT_EQUAL_STRING("ภาษา", w);
	free(w);
}

void test_word_thai_middle_word_bounded_by_zwsp(void) {
	char *w = word_at(thai_row, 15); /* on ไ */
	TEST_ASSERT_EQUAL_STRING("ไทย", w);
	free(w);
}

/* The ZWSP has no screen cell, so the cursor is drawn on the ไ after
 * it: that is the word the user is pointing at. */
void test_word_thai_on_zwsp_takes_word_after(void) {
	char *w = word_at(thai_row, 12); /* on the first ZWSP */
	TEST_ASSERT_EQUAL_STRING("ไทย", w);
	free(w);
}

/* ...but with no word after the separator, fall back to the one
 * before it, as for any cursor just past the end of a word. */
void test_word_zwsp_at_end_of_row_takes_word_before(void) {
	char *w = word_at("ภาษา" ZW, 12);
	TEST_ASSERT_EQUAL_STRING("ภาษา", w);
	free(w);
}

void test_word_zwsp_before_space_takes_word_before(void) {
	char *w = word_at("ภาษา" ZW " x", 12);
	TEST_ASSERT_EQUAL_STRING("ภาษา", w);
	free(w);
}

void test_word_thai_with_tone_mark_at_end_of_row(void) {
	int len = (int)strlen(thai_row);
	char *w = word_at(thai_row, len); /* end of row, after ย */
	TEST_ASSERT_EQUAL_STRING("ง่าย", w);
	free(w);
}

/* The editor knows no Thai: MAI YAMOK is part of the word unless the
 * text marks a boundary before it with a ZWSP. */
void test_word_thai_mai_yamok_is_part_of_word(void) {
	char *w = word_at("ต่างๆ", 0);
	TEST_ASSERT_EQUAL_STRING("ต่างๆ", w);
	free(w);
}

void test_word_thai_zwsp_before_mai_yamok_is_a_boundary(void) {
	char *w = word_at("ต่าง" ZW "ๆ", 0);
	TEST_ASSERT_EQUAL_STRING("ต่าง", w);
	free(w);
}

/* ZERO WIDTH NON-JOINER sits inside words, so unlike ZWSP it must not
 * end one.  "می" ZWNJ "خواهم" is a single Persian word (17 bytes). */
#define ZWNJ "\xe2\x80\x8c"
#define ZWJ "\xe2\x80\x8d"
static const char *persian_row = "می" ZWNJ "خواهم";

void test_word_zwnj_does_not_end_word_from_start(void) {
	char *w = word_at(persian_row, 0);
	TEST_ASSERT_EQUAL_STRING(persian_row, w);
	free(w);
}

void test_word_zwnj_does_not_end_word_from_after_it(void) {
	char *w = word_at(persian_row, 7); /* on خ, just after the ZWNJ */
	TEST_ASSERT_EQUAL_STRING(persian_row, w);
	free(w);
}

void test_word_on_zwnj_takes_whole_word(void) {
	char *w = word_at(persian_row, 4); /* on the ZWNJ itself */
	TEST_ASSERT_EQUAL_STRING(persian_row, w);
	free(w);
}

void test_word_zwj_does_not_end_word(void) {
	/* Devanagari क + VIRAMA + ZWJ + ष: one conjunct, one word. */
	const char *row = "क्" ZWJ "ष";
	char *w = word_at(row, 0);
	TEST_ASSERT_EQUAL_STRING(row, w);
	free(w);
}

void test_word_zwsp_still_ends_word_next_to_zwnj(void) {
	/* The carve-out is for ZWNJ/ZWJ only: ZWSP still separates. */
	char *w = word_at("می" ZWNJ "خواهم" ZW "ab", 0);
	TEST_ASSERT_EQUAL_STRING(persian_row, w);
	free(w);
}

void test_word_thai_paiyannoi_is_part_of_word(void) {
	char *w = word_at("กรุงเทพฯ", 0);
	TEST_ASSERT_EQUAL_STRING("กรุงเทพฯ", w);
	free(w);
}

void test_word_non_ascii_digits_are_word_characters(void) {
	char *w = word_at("ปี๒๕๖๙", 0);
	TEST_ASSERT_EQUAL_STRING("ปี๒๕๖๙", w);
	free(w);
}

void test_word_thai_next_to_ascii(void) {
	char *w = word_at("(ภาษา)", 1);
	TEST_ASSERT_EQUAL_STRING("ภาษา", w);
	free(w);
}

void test_word_accented_latin_from_ascii_letter(void) {
	char *w = word_at("caf\xc3\xa9 au lait", 1); /* on the a */
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", w);
	free(w);
}

void test_word_accented_latin_from_accented_letter(void) {
	char *w = word_at("caf\xc3\xa9 au lait", 3); /* on the e-acute */
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", w);
	free(w);
}

void test_word_greek(void) {
	/* "Ελλάδα x", cursor on the second letter */
#define GREECE "\xce\x95\xce\xbb\xce\xbb\xce\xac\xce\xb4\xce\xb1"
	char *w = word_at(GREECE " x", 2);
	TEST_ASSERT_EQUAL_STRING(GREECE, w);
#undef GREECE
	free(w);
}

/* Curly quotes and dashes are General Punctuation, so they end a word
 * rather than becoming part of it. */
void test_word_curly_quotes_are_separators(void) {
	char *w = word_at("\xe2\x80\x9c" "caf\xc3\xa9" "\xe2\x80\x9d", 4);
	TEST_ASSERT_EQUAL_STRING("caf\xc3\xa9", w);
	free(w);
}

void test_word_em_dash_is_a_separator(void) {
	char *w = word_at("HOT\xe2\x80\x94" "DOG", 0);
	TEST_ASSERT_EQUAL_STRING("HOT", w);
	free(w);
}

void test_word_guillemets_are_separators(void) {
	char *w = word_at("\xc2\xab" "na\xc3\xafve" "\xc2\xbb", 2);
	TEST_ASSERT_EQUAL_STRING("na\xc3\xafve", w);
	free(w);
}

/* A lead byte with its continuation bytes missing (reachable through
 * byte-column rectangle edits) is one codepoint, not a licence to skip
 * the bytes that follow it. */
void test_word_truncated_sequence_does_not_swallow_following_bytes(void) {
	char *w = word_at("\xe0 x", 0);
	TEST_ASSERT_EQUAL_STRING("\xe0", w);
	free(w);
}

/* ---- ctagsParseLine ---- */

static int parse(const char *text, const char *sym, struct tagMatch *m,
		 char *line, size_t linesz) {
	emil_strlcpy(line, text, linesz);
	return ctagsParseLine(line, sym, m);
}

void test_parse_pattern_address_and_scope(void) {
	char line[256];
	struct tagMatch m;
	int rc = parse("close\tasyncio/streams.py\t/^    def close(self):$/;\""
		       "\tm\tclass:StreamWriter\n",
		       "close", &m, line, sizeof(line));
	TEST_ASSERT_EQUAL_INT(0, rc);
	TEST_ASSERT_EQUAL_STRING("asyncio/streams.py", m.file);
	TEST_ASSERT_EQUAL_STRING("    def close(self):", m.pat);
	TEST_ASSERT_EQUAL_STRING("StreamWriter", m.scope);
	TEST_ASSERT_EQUAL_INT(0, m.line);
}

void test_parse_number_address(void) {
	char line[256];
	struct tagMatch m;
	TEST_ASSERT_EQUAL_INT(0, parse("foo\tfoo.c\t42;\"\tf\n", "foo", &m,
				       line, sizeof(line)));
	TEST_ASSERT_EQUAL_INT(42, m.line);
	TEST_ASSERT_EQUAL_STRING("", m.pat);
	TEST_ASSERT_NULL(m.scope);
}

/* ctags --excmd=combine: the number is what tells apart two methods
 * whose definition lines are identical. */
void test_parse_combined_address(void) {
	char line[256];
	struct tagMatch m;
	TEST_ASSERT_EQUAL_INT(0, parse("foo\tfoo.c\t357;/^int foo(void)$/;\"\tf",
				       "foo", &m, line, sizeof(line)));
	TEST_ASSERT_EQUAL_INT(357, m.line);
	TEST_ASSERT_EQUAL_STRING("int foo(void)", m.pat);
}

void test_parse_line_field(void) {
	char line[256];
	struct tagMatch m;
	TEST_ASSERT_EQUAL_INT(0, parse("x\ta.py\t/^x = 1$/;\"\tv\tline:12\n",
				       "x", &m, line, sizeof(line)));
	TEST_ASSERT_EQUAL_INT(12, m.line);
	TEST_ASSERT_EQUAL_STRING("x = 1", m.pat);
}

void test_parse_scope_field_form(void) {
	char line[256];
	struct tagMatch m;
	TEST_ASSERT_EQUAL_INT(0, parse("run\ta.py\t9;\"\tm\tscope:class:Task\n",
				       "run", &m, line, sizeof(line)));
	TEST_ASSERT_EQUAL_STRING("Task", m.scope);
}

void test_parse_typeref_is_not_a_scope(void) {
	char line[256];
	struct tagMatch m;
	TEST_ASSERT_EQUAL_INT(0, parse("n\ta.c\t3;\"\tv\ttyperef:typename:int\n",
				       "n", &m, line, sizeof(line)));
	TEST_ASSERT_NULL(m.scope);
}

void test_parse_escaped_delimiter_and_crlf(void) {
	char line[256];
	struct tagMatch m;
	TEST_ASSERT_EQUAL_INT(0, parse("d\td.c\t/^a\\/b \\\\ c$/;\"\tf\r\n", "d",
				       &m, line, sizeof(line)));
	TEST_ASSERT_EQUAL_STRING("a/b \\ c", m.pat);
}

/* Format 1, as a Thai vocabulary tags file writes it: no fields. */
void test_parse_format1_dictionary_line(void) {
	char line[256];
	struct tagMatch m;
	TEST_ASSERT_EQUAL_INT(0, parse("กรรม\tdict.tsv\t1\n", "กรรม", &m, line,
				       sizeof(line)));
	TEST_ASSERT_EQUAL_STRING("dict.tsv", m.file);
	TEST_ASSERT_EQUAL_INT(1, m.line);
}

void test_parse_rejects_other_names_and_pseudotags(void) {
	char line[256];
	struct tagMatch m;
	TEST_ASSERT_EQUAL_INT(-1, parse("closed\ta.py\t1\n", "close", &m, line,
					sizeof(line)));
	TEST_ASSERT_EQUAL_INT(-1, parse("!_TAG_FILE_SORTED\t1\t//\n", "!_TAG",
					&m, line, sizeof(line)));
}

/* ---- ctagsJump: one match jumps, several open the menu ---- */

static void write_file(const char *dir, const char *name, const char *text) {
	char path[PATH_MAX + 32];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *fp = fopen(path, "w");
	if (fp) {
		fputs(text, fp);
		fclose(fp);
	}
}

/* 'close' three times: twice in a.py with identical definition lines,
 * so only the line numbers tell classes A and B apart, and once in
 * b.py.  'helper' once. */
static char menu_root[PATH_MAX];
static char menu_saved_cwd[PATH_MAX];

static int enter_menu_project(void) {
	char tmpl[] = "/tmp/emil_tagmenu_XXXXXX";
	char *made = mkdtemp(tmpl);
	if (!made || emil_strlcpy(menu_root, made, sizeof(menu_root)) >=
			     sizeof(menu_root))
		return -1;
	write_file(menu_root, "a.py",
		   "class A:\n    def close(self):\n        pass\n"
		   "class B:\n    def close(self):\n        pass\n");
	write_file(menu_root, "b.py",
		   "class C:\n    def close(self):\n        pass\n"
		   "def helper():\n    pass\n");
	write_file(menu_root, "tags",
		   "!_TAG_FILE_FORMAT\t2\t//\n"
		   "!_TAG_FILE_SORTED\t1\t//\n"
		   "close\ta.py\t2;/^    def close(self):$/;\"\tm\tclass:A\n"
		   "close\ta.py\t5;/^    def close(self):$/;\"\tm\tclass:B\n"
		   "close\tb.py\t2;/^    def close(self):$/;\"\tm\tclass:C\n"
		   "helper\tb.py\t/^def helper():$/;\"\tf\n");
	if (!getcwd(menu_saved_cwd, sizeof(menu_saved_cwd)) ||
	    chdir(menu_root) != 0)
		return -1;
	return 0;
}

static void leave_menu_project(void) {
	TEST_ASSERT_EQUAL_INT(0, chdir(menu_saved_cwd));
	rm_project(menu_root);
}

/* Press M-. on 'close' in "x.close()" with the given keys queued. */
static void jump_with_keys(const char *line, int cx, const int *keys, int n) {
	struct buffer *buf = make_test_buffer(line);
	buf->cx = cx;
	scriptKeys(keys, n);
	muteStdout();
	ctagsJump();
	unmuteStdout();
}

static int ends_with(const char *s, const char *suffix) {
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* ---- ctagsJump on a marked region ---- */

static char reg_root[PATH_MAX];
static char reg_saved_cwd[PATH_MAX];

static int enter_region_project(void) {
	char tmpl[] = "/tmp/emil_tagregion_XXXXXX";
	char *made = mkdtemp(tmpl);
	if (!made || emil_strlcpy(reg_root, made, sizeof(reg_root)) >=
			     sizeof(reg_root))
		return -1;
	write_file(reg_root, "lib.lisp",
		   "(defvar *debug* nil)\n"
		   "(defun set-car! (c v) v)\n"
		   "(defun car (c) c)\n");
	write_file(reg_root, "foo.cpp", "int x;\nvoid Foo::bar() {}\n");
	write_file(reg_root, "tags",
		   "!_TAG_FILE_FORMAT\t2\t//\n"
		   "!_TAG_FILE_SORTED\t1\t//\n"
		   "*debug*\tlib.lisp\t1;\"\tv\n"
		   "Foo::bar\tfoo.cpp\t2;\"\tf\n"
		   "car\tlib.lisp\t3;\"\tf\n"
		   "set-car!\tlib.lisp\t2;\"\tf\n");
	if (!getcwd(reg_saved_cwd, sizeof(reg_saved_cwd)) ||
	    chdir(reg_root) != 0)
		return -1;
	return 0;
}

static void leave_region_project(void) {
	TEST_ASSERT_EQUAL_INT(0, chdir(reg_saved_cwd));
	rm_project(reg_root);
}

/* Mark [markx, cx) on a one-line buffer and jump. */
static struct buffer *jump_marked(const char *line, int markx, int cx) {
	struct buffer *buf = make_test_buffer(line);
	buf->markx = markx;
	buf->marky = 0;
	buf->cx = cx;
	buf->mark_active = 1;
	int keys[] = { CTRL('g') }; /* would cancel a menu, if one opened */
	scriptKeys(keys, 1);
	muteStdout();
	ctagsJump();
	unmuteStdout();
	return buf;
}

void test_region_jump_lisp_earmuffs(void) {
	if (enter_region_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	jump_marked("(when *debug* (print x))", 6, 13);
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "lib.lisp"));
	TEST_ASSERT_EQUAL_INT(0, E.buf->cy);
	leave_region_project();
}

/* Mark set after point, with blanks around the name. */
void test_region_jump_backward_mark_trims_blanks(void) {
	if (enter_region_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	jump_marked("call  Foo::bar  now", 15, 4);
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "foo.cpp"));
	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	leave_region_project();
}

void test_region_jump_lisp_bang(void) {
	if (enter_region_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	jump_marked("(set-car! p 1)", 1, 9);
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "lib.lisp"));
	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	leave_region_project();
}

/* A marked name is exact: no fallback to the word at point. */
void test_region_jump_no_fallback_to_word(void) {
	if (enter_region_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	struct buffer *buf = jump_marked("(car-x 1)", 1, 6);
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_EQUAL_STRING("Tag not found: car-x", E.statusmsg);
	TEST_ASSERT_EQUAL_INT(0, buf->mark_active);
	leave_region_project();
}

void test_region_jump_rejects_multiline(void) {
	if (enter_region_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	const char *lines[] = { "(car", " x)" };
	struct buffer *buf = make_test_buffer_lines(lines, 2);
	buf->markx = 1;
	buf->marky = 0;
	buf->cx = 2;
	buf->cy = 1;
	buf->mark_active = 1;
	muteStdout();
	ctagsJump();
	unmuteStdout();
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_EQUAL_STRING("Tag name can't span lines", E.statusmsg);
	TEST_ASSERT_EQUAL_INT(0, buf->mark_active);
	leave_region_project();
}

void test_region_jump_blank_region(void) {
	if (enter_region_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	struct buffer *buf = jump_marked("car   car", 3, 6);
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_EQUAL_STRING("No symbol in region", E.statusmsg);
	leave_region_project();
}

/* ---- Marked region keeps combining marks with their letter ---- */

/* "ไม่" (not) and "ดี" (good): ไ ม + tone mark, ด + vowel sign above.
 * Every Thai character is 3 bytes.  "café" is decomposed: e + U+0301. */
#define TH_MAI "\xe0\xb9\x84\xe0\xb8\xa1\xe0\xb9\x88"
#define TH_DII "\xe0\xb8\x94\xe0\xb8\xb5"
#define CAFE_NFD "cafe\xcc\x81"

static char th_root[PATH_MAX];
static char th_saved_cwd[PATH_MAX];

static int enter_thai_project(void) {
	char tmpl[] = "/tmp/emil_tagthai_XXXXXX";
	char *made = mkdtemp(tmpl);
	if (!made || emil_strlcpy(th_root, made, sizeof(th_root)) >=
			     sizeof(th_root))
		return -1;
	write_file(th_root, "dict.txt",
		   CAFE_NFD " coffee\n" TH_DII " good\n" TH_MAI " not\n");
	/* Sorted by byte: c < ด < ไ. */
	write_file(th_root, "tags",
		   "!_TAG_FILE_FORMAT\t2\t//\n"
		   "!_TAG_FILE_SORTED\t1\t//\n"
		   CAFE_NFD "\tdict.txt\t1;\"\tw\n"
		   TH_DII "\tdict.txt\t2;\"\tw\n"
		   TH_MAI "\tdict.txt\t3;\"\tw\n");
	if (!getcwd(th_saved_cwd, sizeof(th_saved_cwd)) ||
	    chdir(th_root) != 0)
		return -1;
	return 0;
}

static void leave_thai_project(void) {
	TEST_ASSERT_EQUAL_INT(0, chdir(th_saved_cwd));
	rm_project(th_root);
}

static int jumped_to_dict_line(int line) {
	return E.buf->filename && ends_with(E.buf->filename, "dict.txt") &&
	       E.buf->cy == line;
}

/* Marks only draw at zero width under a UTF-8 locale.  In the C locale
 * (Genode) each takes its own column, so the cursor can't stop before
 * one unseen; the name is taken as marked, which here misses the mark. */
static void assert_mark_lookup(int line) {
	if (combiningCols() == 0) {
		TEST_ASSERT(jumped_to_dict_line(line));
	} else {
		TEST_ASSERT_NULL(E.buf->filename);
		TEST_ASSERT(strncmp(E.statusmsg, "Tag not found", 13) == 0);
	}
}

/* Mark ends between ม and its tone mark: the mark still counts. */
void test_region_thai_end_before_tone_mark(void) {
	if (enter_thai_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	jump_marked(TH_MAI TH_DII, 0, 6);
	assert_mark_lookup(2);
	leave_thai_project();
}

/* Mark starts on the vowel sign above ด: start moves back to ด. */
void test_region_thai_start_on_vowel_sign(void) {
	if (enter_thai_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	jump_marked(TH_MAI TH_DII, 12, 15);
	assert_mark_lookup(1);
	leave_thai_project();
}

/* A ZWSP word break after the word is not pulled into the name. */
void test_region_thai_zwsp_not_absorbed(void) {
	if (enter_thai_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	jump_marked(TH_MAI ZW TH_DII, 0, 9);
	TEST_ASSERT(jumped_to_dict_line(2));
	leave_thai_project();
}

/* Decomposed Latin: a mark ending before the accent keeps it. */
void test_region_decomposed_accent(void) {
	if (enter_thai_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	jump_marked("un " CAFE_NFD " solo", 3, 7);
	assert_mark_lookup(0);
	leave_thai_project();
}

void test_menu_single_match_jumps_directly(void) {
	if (enter_menu_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	int keys[] = { CTRL('g') }; /* would cancel a menu, if one opened */
	jump_with_keys("helper()", 0, keys, 1);
	TEST_ASSERT_EQUAL_INT(0, test_key_pos);
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "b.py"));
	TEST_ASSERT_EQUAL_INT(3, E.buf->cy);
	leave_menu_project();
}

void test_menu_return_visits_first_match(void) {
	if (enter_menu_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	int keys[] = { '\r' };
	jump_with_keys("x.close()", 2, keys, 1);
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "a.py"));
	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	TEST_ASSERT_NULL(findBufferByName("*Tags*"));
	TEST_ASSERT_EQUAL_INT(1, E.nwindows);
	leave_menu_project();
}

/* B.close has the same definition line as A.close: a jump by pattern
 * alone would land on A's. */
void test_menu_second_row_reaches_identical_definition(void) {
	if (enter_menu_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	int keys[] = { CTRL('n'), '\r' };
	jump_with_keys("x.close()", 2, keys, 2);
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "a.py"));
	TEST_ASSERT_EQUAL_INT(4, E.buf->cy);
	leave_menu_project();
}

void test_menu_end_of_list_and_clamping(void) {
	if (enter_menu_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	int keys[] = { KEY_META('>'), CTRL('n'), CTRL('n'), '\r' };
	jump_with_keys("x.close()", 2, keys, 4);
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "b.py"));
	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	leave_menu_project();
}

void test_menu_cancel_leaves_reader_in_place(void) {
	if (enter_menu_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	int keys[] = { CTRL('n'), CTRL('g') };
	jump_with_keys("x.close()", 2, keys, 2);
	TEST_ASSERT_NULL(E.buf->filename);
	TEST_ASSERT_EQUAL_INT(2, E.buf->cx);
	TEST_ASSERT_EQUAL_STRING("Canceled.", E.statusmsg);
	TEST_ASSERT_NULL(findBufferByName("*Tags*"));
	TEST_ASSERT_EQUAL_INT(1, E.nwindows);
	leave_menu_project();
}

/* Reading b.py, its own close is listed first. */
void test_menu_lists_current_file_first(void) {
	if (enter_menu_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	struct buffer *b = switchToFile("b.py");
	TEST_ASSERT_NOT_NULL(b);
	b->cy = 1;
	b->cx = 8; /* on "close" in "    def close(self):" */
	int keys[] = { '\r' };
	scriptKeys(keys, 1);
	muteStdout();
	ctagsJump();
	unmuteStdout();
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "b.py"));
	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	leave_menu_project();
}

void test_menu_cancels_when_macro_runs_out(void) {
	if (enter_menu_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	struct buffer *buf = make_test_buffer("x.close()");
	buf->cx = 2;
	E.macro.keys = xmalloc(sizeof(int));
	E.macro.keys[0] = KEY_META('.');
	E.macro.nkeys = 1;
	E.macro.skeys = 1;
	muteStdout();
	execMacro(&E.macro);
	unmuteStdout();
	TEST_ASSERT_NULL(E.buf->filename);
	TEST_ASSERT_EQUAL_STRING("Canceled.", E.statusmsg);
	TEST_ASSERT_NULL(findBufferByName("*Tags*"));
	leave_menu_project();
}

int main(void) {
	TEST_BEGIN();
	(void)selectUtf8Locale();

	RUN_TEST(test_resolve_relative_joins_onto_tagsdir);
	RUN_TEST(test_resolve_bare_filename);
	RUN_TEST(test_resolve_absolute_passes_through);
	RUN_TEST(test_resolve_home_passes_through);
	RUN_TEST(test_resolve_root_tagsdir);
	RUN_TEST(test_resolve_truncation_reported);
	RUN_TEST(test_resolve_absolute_truncation_reported);
	RUN_TEST(test_find_tags_from_root);
	RUN_TEST(test_find_tags_from_subdir);
	RUN_TEST(test_find_tags_absent);
	RUN_TEST(test_word_ascii_identifier_unchanged);
	RUN_TEST(test_word_ascii_just_after_identifier);
	RUN_TEST(test_word_none_on_punctuation);
	RUN_TEST(test_word_thai_first_word);
	RUN_TEST(test_word_thai_middle_word_bounded_by_zwsp);
	RUN_TEST(test_word_thai_on_zwsp_takes_word_after);
	RUN_TEST(test_word_zwsp_at_end_of_row_takes_word_before);
	RUN_TEST(test_word_zwsp_before_space_takes_word_before);
	RUN_TEST(test_word_thai_with_tone_mark_at_end_of_row);
	RUN_TEST(test_word_thai_mai_yamok_is_part_of_word);
	RUN_TEST(test_word_thai_zwsp_before_mai_yamok_is_a_boundary);
	RUN_TEST(test_word_zwnj_does_not_end_word_from_start);
	RUN_TEST(test_word_zwnj_does_not_end_word_from_after_it);
	RUN_TEST(test_word_on_zwnj_takes_whole_word);
	RUN_TEST(test_word_zwj_does_not_end_word);
	RUN_TEST(test_word_zwsp_still_ends_word_next_to_zwnj);
	RUN_TEST(test_word_thai_paiyannoi_is_part_of_word);
	RUN_TEST(test_word_non_ascii_digits_are_word_characters);
	RUN_TEST(test_word_thai_next_to_ascii);
	RUN_TEST(test_word_accented_latin_from_ascii_letter);
	RUN_TEST(test_word_accented_latin_from_accented_letter);
	RUN_TEST(test_word_greek);
	RUN_TEST(test_word_curly_quotes_are_separators);
	RUN_TEST(test_word_em_dash_is_a_separator);
	RUN_TEST(test_word_guillemets_are_separators);
	RUN_TEST(test_word_truncated_sequence_does_not_swallow_following_bytes);
	RUN_TEST(test_hyphenated_several_single_hyphens);
	RUN_TEST(test_hyphenated_double_hyphen);
	RUN_TEST(test_hyphenated_on_double_hyphen);
	RUN_TEST(test_hyphenated_trailing_hyphens_excluded);
	RUN_TEST(test_hyphenated_spaced_minus_is_not_symbol);
	RUN_TEST(test_region_jump_lisp_earmuffs);
	RUN_TEST(test_region_jump_backward_mark_trims_blanks);
	RUN_TEST(test_region_jump_lisp_bang);
	RUN_TEST(test_region_jump_no_fallback_to_word);
	RUN_TEST(test_region_jump_rejects_multiline);
	RUN_TEST(test_region_jump_blank_region);
	RUN_TEST(test_region_thai_end_before_tone_mark);
	RUN_TEST(test_region_thai_start_on_vowel_sign);
	RUN_TEST(test_region_thai_zwsp_not_absorbed);
	RUN_TEST(test_region_decomposed_accent);
	RUN_TEST(test_parse_pattern_address_and_scope);
	RUN_TEST(test_parse_number_address);
	RUN_TEST(test_parse_combined_address);
	RUN_TEST(test_parse_line_field);
	RUN_TEST(test_parse_scope_field_form);
	RUN_TEST(test_parse_typeref_is_not_a_scope);
	RUN_TEST(test_parse_escaped_delimiter_and_crlf);
	RUN_TEST(test_parse_format1_dictionary_line);
	RUN_TEST(test_parse_rejects_other_names_and_pseudotags);
	RUN_TEST(test_menu_single_match_jumps_directly);
	RUN_TEST(test_menu_return_visits_first_match);
	RUN_TEST(test_menu_second_row_reaches_identical_definition);
	RUN_TEST(test_menu_end_of_list_and_clamping);
	RUN_TEST(test_menu_cancel_leaves_reader_in_place);
	RUN_TEST(test_menu_lists_current_file_first);
	RUN_TEST(test_menu_cancels_when_macro_runs_out);

	return TEST_END();
}
