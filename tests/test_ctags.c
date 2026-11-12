/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_ctags.c: M-. (jump to a tag's definition).
 *
 * Each test builds a small project on disk -- a tags file and the
 * files it names -- puts point on a name, presses M-. and checks
 * which file and line the editor lands on.  Which name was taken from
 * the buffer, how a tags line was read and where the tags file was
 * found all show in where the jump lands, so none of the functions
 * that do those things is called by name and they can be merged or
 * made private without touching this file.
 *
 * The two truncation tests near the top are the exception: a path too
 * long for its buffer cannot be arranged on disk.
 */

#include "test.h"
#include "test_harness.h"
#include "ctags.h" /* resolveTagPath, for the two truncation tests */
#include "unicode.h"
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
	makeMinibuffer();
}
void tearDown(void) {
	freeMinibuffer();
	cleanupTestEditor();
}

/* Several jumps in one test each start from a clean editor. */
static void reset_editor(void) {
	freeMinibuffer();
	cleanupTestEditor();
	initTestEditor();
	makeMinibuffer();
}

/* ---- Projects on disk ---- */

static void write_file(const char *dir, const char *name, const char *text) {
	char path[PATH_MAX + 32];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *fp = fopen(path, "w");
	if (fp) {
		fputs(text, fp);
		fclose(fp);
	}
}

static void rm_project(const char *root) {
	char cmd[PATH_MAX + 16];
	snprintf(cmd, sizeof(cmd), "rm -rf %s", root);
	int r = system(cmd);
	(void)r;
}

/* An empty project directory, entered: the working directory is its
 * root until leave_project(). */
static char proj_root[PATH_MAX];
static char proj_saved_cwd[PATH_MAX];

static int enter_project(void) {
	char tmpl[PATH_MAX];
	testTmpPath(tmpl, sizeof(tmpl), "emil_ctags_XXXXXX");
	char *made = mkdtemp(tmpl);
	if (!made || emil_strlcpy(proj_root, made, sizeof(proj_root)) >=
			     sizeof(proj_root))
		return -1;
	if (!getcwd(proj_saved_cwd, sizeof(proj_saved_cwd)) ||
	    chdir(proj_root) != 0)
		return -1;
	return 0;
}

static void leave_project(void) {
	TEST_ASSERT_EQUAL_INT(0, chdir(proj_saved_cwd));
	rm_project(proj_root);
}

/* ---- Keys ---- */

/* How many of the waiting keys the last jump read. */
static int keys_read;

/* M-., with `keys` waiting for the menu if one opens. */
static void press_jump(const int *keys, int n) {
	scriptKeys(keys, n);
	muteStdout();
	processKeypress(CMD_CTAGS_JUMP);
	unmuteStdout();
	keys_read = test_key_pos;
	clearKeys();
}

/* M-. where no menu is expected.  A C-g waits all the same, so a menu
 * that does open is cancelled, not left waiting for ever. */
static void press_jump_plain(void) {
	int keys[] = { KEY_CTRL('g') };
	press_jump(keys, 1);
}

/* ---- Where the jump landed ---- */

static int ends_with(const char *s, const char *suffix) {
	size_t a = strlen(s), b = strlen(suffix);
	return a >= b && strcmp(s + a - b, suffix) == 0;
}

/* Is the editor showing `file` with point on its 1-based `line`,
 * and does that line read `text`? */
static int landed_on(const char *file, int line, const char *text) {
	return E.buf->filename && ends_with(E.buf->filename, file) &&
	       E.buf->cy == line - 1 && E.buf->cy < E.buf->numrows &&
	       strcmp(row_str(E.buf, E.buf->cy), text) == 0;
}

static struct buffer *buffer_named(const char *name) {
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next)
		if (b->filename && strcmp(b->filename, name) == 0)
			return b;
	return NULL;
}

/* Is `want` somewhere in the frame last drawn? */
static int frame_has(const char *want) {
	int n = (int)strlen(want);
	for (int i = 0; i + n <= E.render_buf.len; i++)
		if (memcmp(E.render_buf.b + i, want, n) == 0)
			return 1;
	return 0;
}

/* ---- Joining a tag's path onto the tags directory ---- */

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

/* An absolute path in the tags file is used as it stands, not joined
 * onto the project directory. */
void test_resolve_absolute_passes_through(void) {
	char other[PATH_MAX];
	testTmpPath(other, sizeof(other), "emil_ctags_abs_XXXXXX");
	TEST_ASSERT_NOT_NULL(mkdtemp(other));
	write_file(other, "target.txt", "first\nabsolute target\n");
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	char tags[2 * PATH_MAX];
	snprintf(tags, sizeof(tags), "sym\t%s/target.txt\t2;\"\tv\n", other);
	write_file(proj_root, "tags", tags);

	make_test_buffer("sym");
	press_jump_plain();

	TEST_ASSERT(landed_on("target.txt", 2, "absolute target"));
	leave_project();
	rm_project(other);
}

/* A path under ~ is left for the editor to expand, so it lands in the
 * home directory whatever the project is. */
void test_resolve_home_passes_through(void) {
	char home[PATH_MAX];
	testTmpPath(home, sizeof(home), "emil_ctags_home_XXXXXX");
	TEST_ASSERT_NOT_NULL(mkdtemp(home));
	write_file(home, "target.txt", "first\nhome target\n");
	const char *old = getenv("HOME");
	char *saved_home = old ? xstrdup(old) : NULL;
	TEST_ASSERT_EQUAL_INT(0, setenv("HOME", home, 1));
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_file(proj_root, "tags", "sym\t~/target.txt\t2;\"\tv\n");

	make_test_buffer("sym");
	press_jump_plain();

	TEST_ASSERT(landed_on("target.txt", 2, "home target"));
	leave_project();
	if (saved_home)
		setenv("HOME", saved_home, 1);
	else
		unsetenv("HOME");
	free(saved_home);
	rm_project(home);
}

/* ---- Finding the tags file from below the project root ---- */

void test_find_tags_from_subdir(void) {
	/* The case that used to fail entirely: the working directory
	 * is two levels down, the tags file is at the root, and the
	 * path in it is relative to the root. */
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	char path[PATH_MAX + 16];
	snprintf(path, sizeof(path), "%s/src", proj_root);
	mkdir(path, 0755);
	snprintf(path, sizeof(path), "%s/src/deep", proj_root);
	mkdir(path, 0755);
	write_file(proj_root, "tags",
		   "helper_fn\tsrc/foo.c\t/^int helper_fn(void)$/;\"\tf\n");
	write_file(proj_root, "src/foo.c",
		   "#include <x.h>\nint helper_fn(void)\n{\n}\n");
	TEST_ASSERT_EQUAL_INT(0, chdir(path));

	make_test_buffer("helper_fn();");
	press_jump_plain();

	TEST_ASSERT(landed_on("src/foo.c", 2, "int helper_fn(void)"));
	leave_project();
}

void test_find_tags_absent(void) {
	/* A directory tree with no tags file anywhere up to root: the
	 * jump goes nowhere.
	 *
	 * NOTE: this assumes no "tags" file exists in any ancestor of
	 * the temp directory, which holds on normal systems. */
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_file(proj_root, "foo.c", "int helper_fn(void)\n");

	struct buffer *buf = make_test_buffer("helper_fn();");
	buf->cx = 3;
	press_jump_plain();

	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_EQUAL_INT(3, buf->cx);
	TEST_ASSERT_EQUAL_INT(0, buf->cy);
	TEST_ASSERT_EQUAL_STRING("helper_fn();", row_str(buf, 0));
	TEST_ASSERT(strlen(E.statusmsg) > 0);
	leave_project();
}

/* ---- Which name M-. takes from the buffer ----
 *
 * The name is never read back.  The project's tags file defines the
 * name the editor ought to take, at line 3 of defs.txt, and each
 * wrong name a faulty scan might produce at line 1 of decoy.txt.
 * Landing on line 3 of defs.txt means exactly the right name was
 * looked up; a tags entry matches only a whole name. */

/* M-. at byte `cx` of `line`.  `want` is the name that must be taken,
 * or NULL when none may be; `decoys` is a NULL-terminated list of
 * wrong names, or NULL. */
static void assert_symbol(const char *line, int cx, const char *want,
			  const char *const *decoys) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_file(proj_root, "defs.txt", "one\ntwo\nthree\n");
	write_file(proj_root, "decoy.txt", "decoy\n");
	char path[PATH_MAX + 32];
	snprintf(path, sizeof(path), "%s/tags", proj_root);
	FILE *fp = fopen(path, "w");
	TEST_ASSERT_NOT_NULL(fp);
	if (fp) {
		if (want)
			fprintf(fp, "%s\tdefs.txt\t3;\"\tv\n", want);
		for (int i = 0; decoys && decoys[i]; i++)
			fprintf(fp, "%s\tdecoy.txt\t1;\"\tv\n", decoys[i]);
		fclose(fp);
	}

	struct buffer *buf = make_test_buffer(line);
	buf->cx = cx;
	press_jump_plain();

	if (want) {
		TEST_ASSERT(landed_on("defs.txt", 3, "three"));
	} else {
		TEST_ASSERT(E.buf == buf);
		TEST_ASSERT_EQUAL_INT(cx, buf->cx);
	}
	TEST_ASSERT_EQUAL_INT(0, keys_read);
	leave_project();
	reset_editor();
}

/* ---- Lisp-style symbols: words joined by hyphens ----
 *
 * The plain word under point is the decoy each time: it is what a
 * scan that stopped at the hyphen would look up. */

void test_hyphenated_several_single_hyphens(void) {
	const char *const decoys[] = { "foo", "bar", "baz", NULL };
	assert_symbol("(foo-bar-baz x)", 1, "foo-bar-baz", decoys);
	assert_symbol("(foo-bar-baz x)", 6, "foo-bar-baz", decoys);
	assert_symbol("(foo-bar-baz x)", 10, "foo-bar-baz", decoys);
}

/* Emacs Lisp names internal functions pkg--name. */
void test_hyphenated_double_hyphen(void) {
	const char *const decoys[] = { "my", "pkg", "helper", "my-pkg", NULL };
	assert_symbol("(my-pkg--helper x)", 1, "my-pkg--helper", decoys);
	assert_symbol("(my-pkg--helper x)", 5, "my-pkg--helper", decoys);
	assert_symbol("(my-pkg--helper x)", 10, "my-pkg--helper", decoys);
}

/* Point on the first hyphen, just after a word. */
void test_hyphenated_on_double_hyphen(void) {
	const char *const decoys[] = { "pkg", "my-pkg", "helper", NULL };
	assert_symbol("(my-pkg--helper x)", 7, "my-pkg--helper", decoys);
}

/* Hyphens not followed by a word stay out of the symbol. */
void test_hyphenated_trailing_hyphens_excluded(void) {
	const char *const after[] = { "foo-bar--", "foo-bar-", "foo", NULL };
	assert_symbol("foo-bar-- x", 1, "foo-bar", after);
	const char *const before[] = { "--foo-bar", "-foo-bar", "foo", NULL };
	assert_symbol("x --foo-bar", 4, "foo-bar", before);
}

/* C: a - b is three things, not one symbol.  The plain word is taken. */
void test_hyphenated_spaced_minus_is_not_symbol(void) {
	const char *const decoys[] = { "a - b", "a-b", "a -", "a-", NULL };
	assert_symbol("a - b", 0, "a", decoys);
}

/* ---- Words: ASCII identifiers and non-ASCII words ---- */

/* "ภาษา" ZWSP "ไทย" ZWSP "ง่าย" -- three words, 12 + 3 + 9 + 3 + 12
 * bytes.  Offsets below are byte offsets into this row. */
#define ZW "\xe2\x80\x8b"
static const char *thai_row = "ภาษา" ZW "ไทย" ZW "ง่าย";

void test_word_ascii_identifier_unchanged(void) {
	const char *const decoys[] = { "foo", "bar", "foo_bar(x)", NULL };
	assert_symbol("foo_bar(x)", 2, "foo_bar", decoys);
}

void test_word_ascii_just_after_identifier(void) {
	const char *const decoys[] = { "x", "foo_bar(", NULL };
	assert_symbol("foo_bar(x)", 7, "foo_bar", decoys);
}

void test_word_none_on_punctuation(void) {
	const char *const decoys[] = { "a", "b", "+", "a + b", NULL };
	assert_symbol("a + b", 2, NULL, decoys);
}

void test_word_thai_first_word(void) {
	/* on the second codepoint */
	assert_symbol(thai_row, 3, "ภาษา", NULL);
}

void test_word_thai_middle_word_bounded_by_zwsp(void) {
	const char *const decoys[] = { "ภาษา", "ง่าย", NULL };
	assert_symbol(thai_row, 15, "ไทย", decoys); /* on ไ */
}

/* The ZWSP has no screen cell, so the cursor is drawn on the ไ after
 * it: that is the word the user is pointing at. */
void test_word_thai_on_zwsp_takes_word_after(void) {
	const char *const decoys[] = { "ภาษา", NULL };
	assert_symbol(thai_row, 12, "ไทย", decoys); /* on the first ZWSP */
}

/* ...but with no word after the separator, fall back to the one
 * before it, as for any cursor just past the end of a word. */
void test_word_zwsp_at_end_of_row_takes_word_before(void) {
	assert_symbol("ภาษา" ZW, 12, "ภาษา", NULL);
}

void test_word_zwsp_before_space_takes_word_before(void) {
	const char *const decoys[] = { "x", NULL };
	assert_symbol("ภาษา" ZW " x", 12, "ภาษา", decoys);
}

void test_word_thai_with_tone_mark_at_end_of_row(void) {
	int len = (int)strlen(thai_row);
	const char *const decoys[] = { "ไทย", NULL };
	/* end of row, after ย */
	assert_symbol(thai_row, len, "ง่าย", decoys);
}

/* The editor knows no Thai: MAI YAMOK is part of the word unless the
 * text marks a boundary before it with a ZWSP. */
void test_word_thai_mai_yamok_is_part_of_word(void) {
	const char *const decoys[] = { "ต่าง", NULL };
	assert_symbol("ต่างๆ", 0, "ต่างๆ", decoys);
}

void test_word_thai_zwsp_before_mai_yamok_is_a_boundary(void) {
	const char *const decoys[] = { "ต่างๆ", "ต่าง" ZW "ๆ", NULL };
	assert_symbol("ต่าง" ZW "ๆ", 0, "ต่าง", decoys);
}

/* ZERO WIDTH NON-JOINER sits inside words, so unlike ZWSP it must not
 * end one.  "می" ZWNJ "خواهم" is a single Persian word (17 bytes). */
#define ZWNJ "\xe2\x80\x8c"
#define ZWJ "\xe2\x80\x8d"
static const char *persian_row = "می" ZWNJ "خواهم";
static const char *const persian_halves[] = { "می", "خواهم", NULL };

void test_word_zwnj_does_not_end_word_from_start(void) {
	assert_symbol(persian_row, 0, persian_row, persian_halves);
}

void test_word_zwnj_does_not_end_word_from_after_it(void) {
	/* on خ, just after the ZWNJ */
	assert_symbol(persian_row, 7, persian_row, persian_halves);
}

void test_word_on_zwnj_takes_whole_word(void) {
	/* on the ZWNJ itself */
	assert_symbol(persian_row, 4, persian_row, persian_halves);
}

void test_word_zwj_does_not_end_word(void) {
	/* Devanagari क + VIRAMA + ZWJ + ष: one conjunct, one word. */
	const char *row = "क्" ZWJ "ष";
	const char *const decoys[] = { "क्", "ष", NULL };
	assert_symbol(row, 0, row, decoys);
}

void test_word_zwsp_still_ends_word_next_to_zwnj(void) {
	/* The carve-out is for ZWNJ/ZWJ only: ZWSP still separates. */
	const char *const decoys[] = { "می" ZWNJ "خواهم" ZW "ab", "ab", NULL };
	assert_symbol("می" ZWNJ "خواهم" ZW "ab", 0, persian_row, decoys);
}

void test_word_thai_paiyannoi_is_part_of_word(void) {
	const char *const decoys[] = { "กรุงเทพ", NULL };
	assert_symbol("กรุงเทพฯ", 0, "กรุงเทพฯ", decoys);
}

void test_word_non_ascii_digits_are_word_characters(void) {
	const char *const decoys[] = { "ปี", NULL };
	assert_symbol("ปี๒๕๖๙", 0, "ปี๒๕๖๙", decoys);
}

void test_word_thai_next_to_ascii(void) {
	const char *const decoys[] = { "(ภาษา)", "(ภาษา", "ภาษา)", NULL };
	assert_symbol("(ภาษา)", 1, "ภาษา", decoys);
}

void test_word_accented_latin_from_ascii_letter(void) {
	const char *const decoys[] = { "caf", "au", NULL };
	/* on the a */
	assert_symbol("caf\xc3\xa9 au lait", 1, "caf\xc3\xa9", decoys);
}

void test_word_accented_latin_from_accented_letter(void) {
	const char *const decoys[] = { "caf", "\xc3\xa9", NULL };
	/* on the e-acute */
	assert_symbol("caf\xc3\xa9 au lait", 3, "caf\xc3\xa9", decoys);
}

void test_word_greek(void) {
	/* "Ελλάδα x", cursor on the second letter */
#define GREECE "\xce\x95\xce\xbb\xce\xbb\xce\xac\xce\xb4\xce\xb1"
	const char *const decoys[] = { "x", NULL };
	assert_symbol(GREECE " x", 2, GREECE, decoys);
#undef GREECE
}

/* Curly quotes and dashes are General Punctuation, so they end a word
 * rather than becoming part of it. */
void test_word_curly_quotes_are_separators(void) {
	const char *const decoys[] = { "\xe2\x80\x9c"
				       "caf\xc3\xa9"
				       "\xe2\x80\x9d",
				       "\xe2\x80\x9c"
				       "caf\xc3\xa9",
				       "caf\xc3\xa9"
				       "\xe2\x80\x9d",
				       NULL };
	assert_symbol("\xe2\x80\x9c"
		      "caf\xc3\xa9"
		      "\xe2\x80\x9d",
		      4, "caf\xc3\xa9", decoys);
}

void test_word_em_dash_is_a_separator(void) {
	const char *const decoys[] = { "HOT\xe2\x80\x94"
				       "DOG",
				       "DOG", NULL };
	assert_symbol("HOT\xe2\x80\x94"
		      "DOG",
		      0, "HOT", decoys);
}

void test_word_guillemets_are_separators(void) {
	const char *const decoys[] = { "\xc2\xab"
				       "na\xc3\xafve"
				       "\xc2\xbb",
				       "\xc2\xab"
				       "na\xc3\xafve",
				       "na\xc3\xafve"
				       "\xc2\xbb",
				       NULL };
	assert_symbol("\xc2\xab"
		      "na\xc3\xafve"
		      "\xc2\xbb",
		      2, "na\xc3\xafve", decoys);
}

/* A lead byte with its continuation bytes missing (reachable through
 * byte-column rectangle edits) is one codepoint, not a licence to skip
 * the bytes that follow it. */
void test_word_truncated_sequence_does_not_swallow_following_bytes(void) {
	const char *const decoys[] = { "\xe0 x", "\xe0 ", "x", NULL };
	assert_symbol("\xe0 x", 0, "\xe0", decoys);
}

/* ---- How a tags line is read ----
 *
 * Each test writes one form of tags line and checks that M-. lands
 * where that line says. */

/* A file of `n` lines, "line 1" to "line n". */
static void write_numbered(const char *dir, const char *name, int n) {
	char path[PATH_MAX + 32];
	snprintf(path, sizeof(path), "%s/%s", dir, name);
	FILE *fp = fopen(path, "w");
	if (!fp)
		return;
	for (int i = 1; i <= n; i++)
		fprintf(fp, "line %d\n", i);
	fclose(fp);
}

/* A /pattern/ address finds the line by its text.  The class the tag
 * belongs to is shown in the menu, which opens once there is a second
 * definition to choose from. */
void test_parse_pattern_address_and_scope(void) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_file(proj_root, "streams.py",
		   "class StreamWriter:\n"
		   "    x = 1\n"
		   "    def close(self):\n"
		   "        pass\n");
	write_file(proj_root, "other.py", "def close():\n    pass\n");
	write_file(proj_root, "tags",
		   "close\tstreams.py\t/^    def close(self):$/;\""
		   "\tm\tclass:StreamWriter\n"
		   "close\tother.py\t/^def close():$/;\"\tf\n");

	struct buffer *buf = make_test_buffer("x.close()");
	buf->cx = 2;
	int keys[] = { '\r' };
	press_jump(keys, 1);

	TEST_ASSERT_EQUAL_INT(1, keys_read);
	TEST_ASSERT(frame_has("StreamWriter"));
	TEST_ASSERT(landed_on("streams.py", 3, "    def close(self):"));
	leave_project();
}

void test_parse_number_address(void) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_numbered(proj_root, "foo.c", 50);
	write_file(proj_root, "tags", "foo\tfoo.c\t42;\"\tf\n");

	make_test_buffer("foo()");
	press_jump_plain();

	TEST_ASSERT(landed_on("foo.c", 42, "line 42"));
	leave_project();
}

/* A line: field gives the line where the address is only a pattern.
 * The same text sits earlier in the file, where a search for the
 * pattern alone would stop. */
void test_parse_line_field(void) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_file(proj_root, "a.py",
		   "# 1\nx = 1\n# 3\n# 4\n# 5\n# 6\n"
		   "# 7\n# 8\n# 9\n# 10\n# 11\nx = 1\n# 13\n");
	write_file(proj_root, "tags", "x\ta.py\t/^x = 1$/;\"\tv\tline:12\n");

	make_test_buffer("x");
	press_jump_plain();

	TEST_ASSERT(landed_on("a.py", 12, "x = 1"));
	leave_project();
}

/* In a pattern the delimiter and the backslash are written escaped,
 * and a tags file from a DOS tool ends its lines with CR LF.  The CR
 * is not part of the last field: the menu shows the class as "Outer",
 * not "Outer^M". */
void test_parse_escaped_delimiter_and_crlf(void) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_file(proj_root, "d.c", "int d;\n// other\na/b \\ c\n");
	write_file(proj_root, "other.c", "int d;\n");
	write_file(proj_root, "tags",
		   "d\td.c\t/^a\\/b \\\\ c$/;\"\tf\tclass:Outer\r\n"
		   "d\tother.c\t1;\"\tf\r\n");

	make_test_buffer("d");
	int keys[] = { '\r' };
	press_jump(keys, 1);

	TEST_ASSERT_EQUAL_INT(1, keys_read);
	TEST_ASSERT(frame_has("Outer"));
	TEST_ASSERT(!frame_has("Outer^M"));
	TEST_ASSERT(landed_on("d.c", 3, "a/b \\ c"));
	leave_project();
}

/* Format 1, as a Thai vocabulary tags file writes it: no fields. */
void test_parse_format1_dictionary_line(void) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_file(proj_root, "dict.tsv", "กรรไกร\tscissors\nกรรม\tkarma\n");
	write_file(proj_root, "tags", "กรรม\tdict.tsv\t2\n");

	make_test_buffer("กรรม");
	press_jump_plain();

	TEST_ASSERT(landed_on("dict.tsv", 2, "กรรม\tkarma"));
	leave_project();
}

/* A tag whose name merely starts with the name sought is another tag,
 * and the !_TAG_ lines that head a tags file are not tags at all. */
void test_parse_rejects_other_names_and_pseudotags(void) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_file(proj_root, "a.py", "closed = 1\n");
	write_file(proj_root, "tags",
		   "!_TAG_FILE_SORTED\t1\t//\n"
		   "closed\ta.py\t1\n");

	struct buffer *buf = make_test_buffer("close()");
	press_jump_plain();
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_NULL(buffer_named("a.py"));

	/* Marking the pseudo-tag's own name asks for it exactly, and
	 * still finds nothing: its "file" is the number 1. */
	reset_editor();
	buf = make_test_buffer("!_TAG_FILE_SORTED");
	buf->markx = 0;
	buf->marky = 0;
	buf->mark_active = 1;
	buf->cx = buf->row[0].size;
	press_jump_plain();
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT_EQUAL_INT(1, E.nwindows);
	TEST_ASSERT_NULL(E.buf->next);
	leave_project();
}

/* ---- One match jumps, several open the menu ---- */

/* 'close' three times: twice in a.py with identical definition lines,
 * so only the line numbers tell classes A and B apart, and once in
 * b.py.  'helper' once. */
static char menu_root[PATH_MAX];
static char menu_saved_cwd[PATH_MAX];

static int enter_menu_project(void) {
	char tmpl[PATH_MAX];
	testTmpPath(tmpl, sizeof(tmpl), "emil_tagmenu_XXXXXX");
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
	press_jump(keys, n);
}

/* ---- M-. on a marked region ---- */

static char reg_root[PATH_MAX];
static char reg_saved_cwd[PATH_MAX];

static int enter_region_project(void) {
	char tmpl[PATH_MAX];
	testTmpPath(tmpl, sizeof(tmpl), "emil_tagregion_XXXXXX");
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
	press_jump_plain();
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
	press_jump_plain();
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
	char tmpl[PATH_MAX];
	testTmpPath(tmpl, sizeof(tmpl), "emil_tagthai_XXXXXX");
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
	int keys[] = { KEY_CTRL('g') }; /* would cancel a menu, if one opened */
	jump_with_keys("helper()", 0, keys, 1);
	TEST_ASSERT_EQUAL_INT(0, keys_read);
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
	TEST_ASSERT_NULL(buffer_named("*Tags*"));
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
	int keys[] = { KEY_CTRL('n'), '\r' };
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
	int keys[] = { KEY_META('>'), KEY_CTRL('n'), KEY_CTRL('n'), '\r' };
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
	int keys[] = { KEY_CTRL('n'), KEY_CTRL('g') };
	jump_with_keys("x.close()", 2, keys, 2);
	TEST_ASSERT_NULL(E.buf->filename);
	TEST_ASSERT_EQUAL_INT(2, E.buf->cx);
	TEST_ASSERT_EQUAL_STRING("Canceled.", E.statusmsg);
	TEST_ASSERT_NULL(buffer_named("*Tags*"));
	TEST_ASSERT_EQUAL_INT(1, E.nwindows);
	leave_menu_project();
}

/* Reading b.py, its own close is listed first. */
void test_menu_lists_current_file_first(void) {
	if (enter_menu_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	struct buffer *scratch = make_test_buffer(NULL);
	struct buffer *b = findFileTyping("b.py");
	TEST_ASSERT(b != scratch);
	b->cy = 1;
	b->cx = 8; /* on "close" in "    def close(self):" */
	int keys[] = { '\r' };
	press_jump(keys, 1);
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "b.py"));
	TEST_ASSERT_EQUAL_INT(1, E.buf->cy);
	leave_menu_project();
}

/* ---- a menu row holds what a row may hold ----
 *
 * A tags file is outside text, so a field can hold bytes a row must
 * not.  Each test writes a tags file with two definitions of `sym`,
 * the first one carrying the awkward bytes, opens the menu, moves the
 * selection off that first row and cancels.  The frame drawn while
 * the menu was open shows the row as the menu holds it. */

static void open_menu_and_cancel(const char *tags) {
	write_file(proj_root, "tags", tags);
	struct buffer *buf = make_test_buffer("sym");
	int keys[] = { KEY_CTRL('n'), KEY_CTRL('g') };
	press_jump(keys, 2);
	TEST_ASSERT_EQUAL_INT(2, keys_read);
	TEST_ASSERT(E.buf == buf);
	TEST_ASSERT(E.render_buf.len > 0);
	TEST_ASSERT(memchr(E.render_buf.b, '\0', E.render_buf.len) == NULL);
}

void test_menu_row_replaces_invalid_bytes_in_every_field(void) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	/* An invalid byte in the scope, the file name and the
	 * definition text.  The second definition's scope is six
	 * columns wide and sets the width of the scope column. */
	open_menu_and_cancel("sym\tqq\xffy.c\t3;/^int\xc3$/;\"\tf\tclass:Cl\xf0\n"
			     "sym\tzz.c\t1;\"\tf\tclass:Sixsix\n");

	/* Cl + one replacement character is 3 wide: 3 of padding, then
	 * the 2 that separate the columns. */
	TEST_ASSERT(frame_has("Cl\xef\xbf\xbd   "
			      "  qq\xef\xbf\xbdy.c:3  int\xef\xbf\xbd"));
	leave_project();
}

void test_menu_row_keeps_valid_text_and_tabs(void) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	/* Valid multibyte text passes through; the blanks that indent
	 * the definition are dropped; the tab inside it stays a tab,
	 * which the screen draws as blank columns and not as ^I. */
	open_menu_and_cancel("sym\t\xe6\x97\xa5.c\t/^\t int\tfoo\xc3\xa9$/;\"\tf\n"
			     "sym\tzz.c\t1;\"\tf\n");

	TEST_ASSERT(frame_has("\xe6\x97\xa5.c  int "));
	TEST_ASSERT(frame_has(" foo\xc3\xa9"));
	TEST_ASSERT(!frame_has("^I"));
	leave_project();
}

void test_menu_row_shows_control_byte_as_caret(void) {
	if (enter_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	open_menu_and_cancel("sym\ta\x01.c\t2;/^x\x7f$/;\"\tf\n"
			     "sym\tzz.c\t1;\"\tf\n");

	TEST_ASSERT(frame_has("a^A.c:2  x^?"));
	leave_project();
}

void test_menu_with_invalid_name_still_reaches_the_valid_row(void) {
	if (enter_menu_project() != 0) {
		TEST_ASSERT(0 && "could not create temp project");
		return;
	}
	write_file(menu_root, "tags",
		   "close\tqq\xffy.py\t1;\"\tm\n"
		   "close\ta.py\t2;\"\tm\n");
	int down[] = { KEY_CTRL('n'), '\r' };
	jump_with_keys("x.close()", 2, down, 2);
	TEST_ASSERT(E.buf->filename && ends_with(E.buf->filename, "a.py"));
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
	TEST_ASSERT_NULL(buffer_named("*Tags*"));
	leave_menu_project();
}

int main(void) {
	TEST_BEGIN();
	(void)selectUtf8Locale();

	RUN_TEST(test_resolve_absolute_passes_through);
	RUN_TEST(test_resolve_home_passes_through);
	RUN_TEST(test_resolve_truncation_reported);
	RUN_TEST(test_resolve_absolute_truncation_reported);
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
	RUN_TEST(test_parse_line_field);
	RUN_TEST(test_parse_escaped_delimiter_and_crlf);
	RUN_TEST(test_parse_format1_dictionary_line);
	RUN_TEST(test_parse_rejects_other_names_and_pseudotags);
	RUN_TEST(test_menu_single_match_jumps_directly);
	RUN_TEST(test_menu_return_visits_first_match);
	RUN_TEST(test_menu_second_row_reaches_identical_definition);
	RUN_TEST(test_menu_end_of_list_and_clamping);
	RUN_TEST(test_menu_cancel_leaves_reader_in_place);
	RUN_TEST(test_menu_lists_current_file_first);
	RUN_TEST(test_menu_row_replaces_invalid_bytes_in_every_field);
	RUN_TEST(test_menu_row_keeps_valid_text_and_tabs);
	RUN_TEST(test_menu_row_shows_control_byte_as_caret);
	RUN_TEST(test_menu_with_invalid_name_still_reaches_the_valid_row);
	RUN_TEST(test_menu_cancels_when_macro_runs_out);

	return TEST_END();
}
