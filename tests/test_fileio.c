/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* test_fileio.c: File I/O, round-trip, UTF-8 validation, emil_getline.
 *
 * Files are opened with C-x C-f (findFileTyping in the harness), saved
 * with C-x C-s and C-x C-w, and reverted with M-x revert-buffer; the
 * tests then read the buffer text and the bytes on disk.  The loader
 * is called by name in three places only, each with a comment saying
 * why no keys reach that case. */

#include "test.h"
#include "test_harness.h"
#include "fileio.h"
#include "util.h"
#include "buffer.h"
#include "keymap.h"
#include "unicode.h"
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>


/* ---- emil_getline ---- */

void test_getline_short(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	fputs("Hello, World!\n", fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(14, r);
	TEST_ASSERT_EQUAL_STRING("Hello, World!\n", line);

	free(line);
	fclose(fp);
}

void test_getline_exact_120(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	for (int i = 0; i < 119; i++)
		fputc('A', fp);
	fputc('\n', fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(120, r);
	TEST_ASSERT(line[119] == '\n');
	TEST_ASSERT(line[120] == '\0');

	free(line);
	fclose(fp);
}

void test_getline_long(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	for (int i = 0; i < 200; i++)
		fputc('0' + (i % 10), fp);
	fputc('\n', fp);
	fputs("Second line\n", fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;

	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(201, r);
	TEST_ASSERT(line[0] == '0');
	TEST_ASSERT(line[199] == '9');

	r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(12, r);
	TEST_ASSERT_EQUAL_STRING("Second line\n", line);

	free(line);
	fclose(fp);
}

void test_getline_no_newline(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	fputs("No newline at end", fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(17, r);
	TEST_ASSERT_EQUAL_STRING("No newline at end", line);

	free(line);
	fclose(fp);
}

void test_getline_empty(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(-1, r);

	free(line);
	fclose(fp);
}

void test_getline_multiple_reallocs(void) {
	FILE *fp = tmpfile();
	TEST_ASSERT_NOT_NULL(fp);
	for (int i = 0; i < 1000; i++)
		fputc('X', fp);
	fputc('\n', fp);
	rewind(fp);

	char *line = NULL;
	size_t n = 0;
	ssize_t r = emil_getline(&line, &n, fp);
	TEST_ASSERT_EQUAL_INT(1001, r);
	TEST_ASSERT(line[0] == 'X');
	TEST_ASSERT(line[999] == 'X');
	TEST_ASSERT(n >= 1001);

	free(line);
	fclose(fp);
}

/* ---- Helpers ---- */

/* A scratch buffer to start from, and the minibuffer the prompts need. */
static struct buffer *startEditing(void) {
	makeMinibuffer();
	setupCommands();
	return make_test_buffer(NULL);
}

/* Write `n` bytes to a fresh temp file; `tmpname` receives its path. */
static void writeBytes(char *tmpname, size_t size, const char *bytes,
		       size_t n) {
	testTmpPath(tmpname, size, "emil_test_XXXXXX");
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	if (n > 0)
		TEST_ASSERT_EQUAL_INT((int)n, (int)write(fd, bytes, n));
	close(fd);
}

/* How many buffers are open. */
static int buffersOpen(void) {
	int n = 0;
	for (struct buffer *b = E.headbuf; b != NULL; b = b->next)
		n++;
	return n;
}

static void typeString(const char *text) {
	for (; *text; text++) {
		E.self_insert_key = (unsigned char)*text;
		processKeypress(CMD_SELF_INSERT);
	}
}

/* Issue a command with `keys` waiting for whatever it asks. */
static void pressWithKeys(int cmd, const int *keys, int n) {
	scriptKeys(keys, n);
	muteStdout();
	processKeypress(cmd);
	unmuteStdout();
	clearKeys();
}

/* M-x name RET */
static void metaX(const char *name) {
	int keys[64];
	int n = 0;
	for (; *name && n < 63; name++)
		keys[n++] = (unsigned char)*name;
	keys[n++] = '\r';
	pressWithKeys(CMD_EXEC_CMD, keys, n);
}

/* The whole buffer as the bytes a save would write. */
static void assertSerialises(struct buffer *buf, const char *want) {
	size_t len = 0;
	char *got = rowsToString(buf, &len);
	TEST_ASSERT_EQUAL_INT((int)strlen(want), (int)len);
	if (len == strlen(want))
		TEST_ASSERT_EQUAL_INT(0, memcmp(want, got, len));
	free(got);
}

/* The bytes of the file at `path`, NUL-terminated; "(missing)" if it
 * is not there. */
static const char *fileBytes(const char *path) {
	static char buf[256];
	FILE *fp = fopen(path, "rb");
	if (!fp)
		return "(missing)";
	size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	fclose(fp);
	return buf;
}

/* C-x C-f on a file that must be refused: the user stays where they
 * were and no buffer is made for it. */
static void assertFindFileRefuses(const char *bytes, size_t n) {
	char tmpname[PATH_MAX];
	writeBytes(tmpname, sizeof(tmpname), bytes, n);
	struct buffer *scratch = startEditing();

	struct buffer *now = findFileTyping(tmpname);

	TEST_ASSERT(now == scratch);
	TEST_ASSERT_EQUAL_INT(1, buffersOpen());
	TEST_ASSERT(strlen(E.statusmsg) > 0);
	freeMinibuffer();
	unlink(tmpname);
}

/* ---- File round-trip ---- */

void test_rows_to_string(void) {
	struct buffer *buf = make_test_buffer(NULL);
	insertRow(buf, 0, (const uint8_t *)"Hello", 5);
	insertRow(buf, 1, (const uint8_t *)"World", 5);
	insertRow(buf, 2, (const uint8_t *)"", 0);

	size_t buflen = 0;
	char *str = rowsToString(buf, &buflen);
	TEST_ASSERT_EQUAL_INT(13, (int)buflen);
	TEST_ASSERT(memcmp(str, "Hello\nWorld\n\n", 13) == 0);

	free(str);
}

/* C-x C-f on a directory opens nothing.  The file prompt keeps the
 * user in the prompt to pick a file inside it; leaving with C-g, they
 * are where they started. */
void test_open_directory_fails(void) {
	char tmpname[PATH_MAX];
	testTmpPath(tmpname, sizeof(tmpname), "emil_test_dir_XXXXXX");
	TEST_ASSERT_NOT_NULL(mkdtemp(tmpname));
	struct buffer *scratch = startEditing();

	struct buffer *now = findFileTyping(tmpname);

	TEST_ASSERT(now == scratch);
	TEST_ASSERT_EQUAL_INT(1, buffersOpen());
	TEST_ASSERT_NULL(scratch->filename);
	assertSerialises(scratch, "");

	freeMinibuffer();
	rmdir(tmpname);
}

/* Regression: a nonexistent path with a trailing '/' names a
 * directory; it used to be offered as a "new file" that could never
 * be saved. */
void test_open_trailing_slash_fails(void) {
	char path[PATH_MAX];
	testTmpPath(path, sizeof(path), "emil_no_such_dir_xyzzy/");
	struct buffer *scratch = startEditing();

	struct buffer *now = findFileTyping(path);

	TEST_ASSERT(now == scratch);
	TEST_ASSERT_EQUAL_INT(1, buffersOpen());
	TEST_ASSERT_NULL(scratch->filename);
	assertSerialises(scratch, "");
	TEST_ASSERT(strlen(E.statusmsg) > 0);

	freeMinibuffer();
}

/* ---- UTF-8 validation ---- */

void test_utf8_valid_file(void) {
	char tmpname[PATH_MAX];
	const char content[] = "Hello \xC2\xA2 \xE2\x82\xAC\n";
	writeBytes(tmpname, sizeof(tmpname), content, 13);
	struct buffer *scratch = startEditing();

	struct buffer *buf = findFileTyping(tmpname);

	TEST_ASSERT(buf != scratch);
	TEST_ASSERT_EQUAL_INT(2, buffersOpen());
	assertSerialises(buf, content);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	freeMinibuffer();
	unlink(tmpname);
}

void test_utf8_invalid_continuation(void) {
	assertFindFileRefuses("Bad \xC2\x41\n", 7);
}

void test_utf8_null_byte_rejected(void) {
	const char data[] = "AB\x00"
			    "CD\n";
	assertFindFileRefuses(data, 6);
}

void test_utf8_truncated_multibyte(void) {
	assertFindFileRefuses("A\xE2\x82\n", 4);
}

/* ---- save() UTF-8 guard ----
 *
 * Every load path refuses files that fail UTF-8 validation, so
 * writing an invalid buffer would produce a file emil itself cannot
 * reopen.  save() must refuse outright with an error message, leave
 * the file untouched, and not mark the buffer clean. */

void test_save_valid_utf8_succeeds(void) {
	char tmpname[PATH_MAX];
	const char content[] = "hello \xE6\x97\xA5\n";
	writeBytes(tmpname, sizeof(tmpname), content, strlen(content));
	startEditing();
	struct buffer *buf = findFileTyping(tmpname);

	/* Edit, then C-x C-s. */
	buf->cx = 0;
	buf->cy = 0;
	typeString("x");
	TEST_ASSERT_TRUE(buf->dirty);
	pressWithKeys(CMD_SAVE, NULL, 0);

	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "Wrote"));
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	/* The file holds exactly what the buffer does. */
	TEST_ASSERT_EQUAL_STRING("xhello \xE6\x97\xA5\n", fileBytes(tmpname));

	freeMinibuffer();
	unlink(tmpname);
}

void test_save_invalid_utf8_refused(void) {
	char tmpname[PATH_MAX];
	testTmpPath(tmpname, sizeof(tmpname), "emil_test_XXXXXX");
	int fd = mkstemp(tmpname);
	TEST_ASSERT(fd >= 0);
	TEST_ASSERT(write(fd, "old", 3) == (ssize_t)3);
	close(fd);

	const char *lines[] = { "abc" };
	struct buffer *buf = make_test_buffer_lines(lines, 1);
	buf->filename = xstrdup(tmpname);
	buf->dirty = 1;
	/* Corrupt the row in place: 0xC2 with no continuation byte */
	buf->row[0].chars[1] = 0xC2;

	save(0);

	/* Refused with an error... */
	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "Save failed"));
	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "invalid UTF-8"));
	/* ...the buffer is still dirty... */
	TEST_ASSERT_EQUAL_INT(1, buf->dirty);
	/* ...and the on-disk file is untouched */
	FILE *fp = fopen(tmpname, "rb");
	TEST_ASSERT_NOT_NULL(fp);
	if (fp) {
		char content[16];
		size_t n = fread(content, 1, sizeof(content), fp);
		fclose(fp);
		TEST_ASSERT_EQUAL_INT(3, (int)n);
		TEST_ASSERT(memcmp(content, "old", 3) == 0);
	}

	unlink(tmpname);
}

/* ---- Saving over a file the buffer knows nothing of ----
 *
 * A buffer opened from a file records what it found (open_mtime), and
 * the pre-save check compares against that.  A buffer with no such
 * record -- the target of C-x C-w, a buffer that has no file yet, a
 * "(New file)" -- used to write straight over whatever was at the path,
 * then delete the backup it had made: the old contents were gone.  It
 * must ask, as Emacs does, and "n" must leave the file alone. */

static char ow_dir[PATH_MAX];

static void owSetup(void) {
	testTmpPath(ow_dir, sizeof(ow_dir), "emil_ow_XXXXXX");
	TEST_ASSERT_NOT_NULL(mkdtemp(ow_dir));
	makeMinibuffer();
}

/* emil spells a path under $HOME as "~/...", and the scratch directory
 * is usually under $HOME.  Compare the paths, not their spelling. */
#define ASSERT_SAME_PATH(want, got)                                     \
	do {                                                            \
		const char *g_ = (got);                                 \
		char *we_ = expandTilde(want);                          \
		char *ge_ = g_ ? expandTilde(g_) : NULL;                \
		TEST_ASSERT_EQUAL_STRING(we_, ge_);                     \
		free(we_);                                              \
		free(ge_);                                              \
	} while (0)

static void owPath(char *out, size_t n, const char *name) {
	int r = snprintf(out, n, "%s/%s", ow_dir, name);
	TEST_ASSERT(r >= 0 && (size_t)r < n);
}

static void owWrite(const char *name, const char *content) {
	char p[PATH_MAX];
	owPath(p, sizeof(p), name);
	FILE *fp = fopen(p, "w");
	TEST_ASSERT_NOT_NULL(fp);
	if (fp) {
		fputs(content, fp);
		fclose(fp);
	}
}

/* The file's contents, or "(missing)". */
static const char *owRead(const char *name) {
	static char buf[256];
	char p[PATH_MAX];
	owPath(p, sizeof(p), name);
	FILE *fp = fopen(p, "r");
	if (!fp)
		return "(missing)";
	size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
	buf[n] = '\0';
	fclose(fp);
	return buf;
}

static void owTeardown(void) {
	const char *names[] = { "a", "b", "c" };
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
		char p[PATH_MAX];
		owPath(p, sizeof(p), names[i]);
		unlink(p);
	}
	rmdir(ow_dir);
	freeMinibuffer();
}

/* Issue `cmd` with the keys for typing `name`'s full path and RET at
 * its prompt, followed by `answer` (0 for none). */
static void owRun(int cmd, const char *name, int answer) {
	char p[PATH_MAX];
	owPath(p, sizeof(p), name);
	int keys[PATH_MAX + 2];
	int n = 0;
	for (const char *q = p; *q; q++)
		keys[n++] = (unsigned char)*q;
	keys[n++] = '\r';
	if (answer)
		keys[n++] = answer;
	pressWithKeys(cmd, keys, n);
}

/* A buffer visiting a, holding "mine", opened with C-x C-f. */
static struct buffer *owVisitA(void) {
	char p[PATH_MAX];
	owWrite("a", "mine\n");
	owPath(p, sizeof(p), "a");
	struct buffer *scratch = make_test_buffer(NULL);
	struct buffer *buf = findFileTyping(p);
	TEST_ASSERT(buf != scratch);
	return buf;
}

void test_save_as_over_existing_file_declined_keeps_it(void) {
	owSetup();
	struct buffer *buf = owVisitA();
	owWrite("b", "precious\n");
	char a[PATH_MAX];
	owPath(a, sizeof(a), "a");

	owRun(CMD_SAVE_AS, "b", 'n');

	TEST_ASSERT_EQUAL_STRING("precious\n", owRead("b"));
	/* Declined before the rename: still visiting a. */
	ASSERT_SAME_PATH(a, buf->filename);
	TEST_ASSERT_EQUAL_STRING("Save aborted.", E.statusmsg);
	owTeardown();
}

void test_save_as_over_existing_file_accepted_writes(void) {
	owSetup();
	struct buffer *buf = owVisitA();
	owWrite("b", "precious\n");
	char b[PATH_MAX];
	owPath(b, sizeof(b), "b");

	owRun(CMD_SAVE_AS, "b", 'y');

	TEST_ASSERT_EQUAL_STRING("mine\n", owRead("b"));
	ASSERT_SAME_PATH(b, buf->filename);
	TEST_ASSERT_FALSE(buf->dirty);
	owTeardown();
}

/* Nothing there: nothing to ask.  A stray prompt would read the
 * scripted keys' end (0) as "n" and abandon the save. */
void test_save_as_to_a_new_name_does_not_ask(void) {
	owSetup();
	owVisitA();

	owRun(CMD_SAVE_AS, "c", 0);

	TEST_ASSERT_EQUAL_STRING("mine\n", owRead("c"));
	owTeardown();
}

/* A buffer that has no file yet, saved to a name that exists. */
void test_first_save_of_unnamed_buffer_over_existing_file_asks(void) {
	owSetup();
	struct buffer *buf = make_test_buffer(NULL);
	typeString("mine");
	TEST_ASSERT_TRUE(buf->dirty);
	owWrite("b", "precious\n");

	owRun(CMD_SAVE, "b", 'n');

	TEST_ASSERT_EQUAL_STRING("precious\n", owRead("b"));
	TEST_ASSERT_NULL(buf->filename);
	TEST_ASSERT_TRUE(buf->dirty);
	owTeardown();
}

/* ---- a file whose name isn't valid text ---- */

static void assert_rows_are_text(const struct buffer *buf) {
	for (int i = 0; i < buf->numrows; i++) {
		const erow *r = &buf->row[i];
		TEST_ASSERT_TRUE(utf8_validate(r->chars, r->size));
		TEST_ASSERT(r->size == 0 || !memchr(r->chars, '\n', r->size));
	}
}

/* Opened, edited, saved: the real path, nothing else.
 *
 * Neither name can be typed at the file prompt, which takes no
 * newline and no escape.  A wildcard reaches them: `wild` is the name
 * with a `?` for each character that cannot be typed, and C-x C-f
 * opens what it matches by its exact path. */
static void check_odd_name_existing(const char *name, const char *wild) {
	owSetup();
	char p[PATH_MAX], typed[PATH_MAX];
	owWrite(name, "old\n");
	owPath(p, sizeof(p), name);
	owPath(typed, sizeof(typed), wild);
	struct buffer *scratch = make_test_buffer(NULL);
	struct buffer *buf = findFileTyping(typed);
	TEST_ASSERT(buf != scratch);
	ASSERT_SAME_PATH(p, buf->filename);
	assert_rows_are_text(buf);

	buf->cx = 0;
	buf->cy = 0;
	typeString("new");
	pressWithKeys(CMD_SAVE, NULL, 0);

	ASSERT_SAME_PATH(p, buf->filename);
	TEST_ASSERT_EQUAL_STRING("newold\n", owRead(name));
	assert_rows_are_text(buf);
	unlink(p);
	owTeardown();
}

/* A new file: the first save creates that exact path.
 *
 * A file that does not exist yet matches no wildcard, so no keys open
 * these: such a name arrives only on the command line.  The loader is
 * called by name here to stand in for that. */
static void check_odd_name_new(const char *name) {
	owSetup();
	char p[PATH_MAX];
	owPath(p, sizeof(p), name);
	struct buffer *buf = make_test_buffer(NULL);
	TEST_ASSERT_EQUAL_INT(0, editorOpen(buf, p));
	buf->cx = 0;
	buf->cy = 0;
	typeString("new");
	pressWithKeys(CMD_SAVE, NULL, 0);
	TEST_ASSERT_EQUAL_STRING("new\n", owRead(name));
	unlink(p);
	owTeardown();
}

void test_lf_name_existing(void) { check_odd_name_existing("x\ny", "x?y"); }
void test_esc_name_existing(void) {
	check_odd_name_existing("x\x1b[0my", "x??0my");
}
void test_lf_name_new(void) { check_odd_name_new("x\ny"); }
void test_esc_name_new(void) { check_odd_name_new("x\x1b[0my"); }

/* Opened as "(New file)"; something else creates the file meanwhile. */
void test_new_file_created_meanwhile_is_not_overwritten_silently(void) {
	owSetup();
	char c[PATH_MAX];
	owPath(c, sizeof(c), "c");
	struct buffer *scratch = make_test_buffer(NULL);
	struct buffer *buf = findFileTyping(c);
	TEST_ASSERT(buf != scratch);
	TEST_ASSERT_EQUAL_STRING("(missing)", owRead("c"));
	typeString("mine");
	TEST_ASSERT_TRUE(buf->dirty);

	owWrite("c", "theirs\n");
	int keys[] = { 'n' };
	pressWithKeys(CMD_SAVE, keys, 1);

	TEST_ASSERT_EQUAL_STRING("theirs\n", owRead("c"));
	TEST_ASSERT_TRUE(buf->dirty);
	owTeardown();
}

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* ---- Final-newline invariant ----
 *
 * A file buffer ends in a newline: its last row is empty unless the
 * buffer is empty.  Established here, at load, so that save() has no
 * policy to apply and never modifies the buffer on the way out.
 *
 * These pin the two ends of that: what a file without a trailing
 * newline becomes on load, and what it costs (nothing) if the user
 * never edits it. */

/* Write the bytes to a temp file and open it with C-x C-f. */
static struct buffer *openBytes(const char *bytes, size_t n, char *tmpname,
				size_t size) {
	writeBytes(tmpname, size, bytes, n);
	struct buffer *scratch = startEditing();
	struct buffer *buf = findFileTyping(tmpname);
	TEST_ASSERT(buf != scratch);
	return buf;
}

/* A file that already ends in a newline is unchanged by the rule. */
void test_load_keeps_existing_final_newline(void) {
	char tmpname[PATH_MAX];
	struct buffer *buf = openBytes("a\nb\n", 4, tmpname, sizeof(tmpname));

	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("", (char *)buf->row[2].chars);
	assertSerialises(buf, "a\nb\n");

	freeMinibuffer();
	unlink(tmpname);
}

/* A file without one gains it at load, not at save. */
void test_load_adds_missing_final_newline(void) {
	char tmpname[PATH_MAX];
	struct buffer *buf = openBytes("a\nb", 3, tmpname, sizeof(tmpname));

	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("b", (char *)buf->row[1].chars);
	TEST_ASSERT_EQUAL_STRING("", (char *)buf->row[2].chars);
	assertSerialises(buf, "a\nb\n");

	freeMinibuffer();
	unlink(tmpname);
}

/* And gaining it does not mark the buffer modified, so an untouched
 * file is not rewritten, prompts nothing on quit, and keeps its mtime.
 * appendRowRaw deliberately leaves the dirty flag alone; this is the
 * property that makes the load-time rule free. */
void test_load_adding_final_newline_leaves_buffer_clean(void) {
	char tmpname[PATH_MAX];
	struct buffer *buf =
		openBytes("no trailing newline", 19, tmpname, sizeof(tmpname));

	assertSerialises(buf, "no trailing newline\n");
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	/* It was not an edit, so there is nothing to undo. */
	processKeypress(CMD_UNDO);
	assertSerialises(buf, "no trailing newline\n");
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);

	freeMinibuffer();
	unlink(tmpname);
}

/* An empty file stays empty: it has no lines, so it has nothing to
 * terminate, and it must not grow a newline out of nowhere. */
void test_load_empty_file_serialises_to_nothing(void) {
	char tmpname[PATH_MAX];
	struct buffer *buf = openBytes("", 0, tmpname, sizeof(tmpname));

	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
	assertSerialises(buf, "");

	freeMinibuffer();
	unlink(tmpname);
}

/* --- Load-time normalisation is reported, not silent ---------------
 *
 * Emil edits UTF-8 text files, and a text file is a sequence of lines
 * each terminated by '\n'.  Input departing from that -- a missing
 * final terminator, or DOS line endings -- is normalised on the way
 * in.  The buffer stays clean, so an unedited file is never rewritten;
 * the status line says what will happen if the user does save. */

/* Open a temp file holding `content` with C-x C-f; the file itself
 * is removed again before returning. */
static struct buffer *openTempWith(const char *content) {
	char tmpname[PATH_MAX];
	writeBytes(tmpname, sizeof(tmpname), content, strlen(content));
	struct buffer *scratch = startEditing();
	struct buffer *buf = findFileTyping(tmpname);
	TEST_ASSERT(buf != scratch);
	freeMinibuffer();
	unlink(tmpname);
	return buf;
}

void test_open_reports_missing_final_newline(void) {
	struct buffer *buf = openTempWith("alpha\nbeta");

	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "no final newline"));
	TEST_ASSERT_NULL(strstr(E.statusmsg, "DOS"));

	/* Normalised in the buffer, but not yet on disk: an untouched
	 * file must not be rewritten, so the buffer stays clean. */
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_INT(0, buf->row[buf->numrows - 1].size);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}

void test_open_reports_dos_line_endings(void) {
	struct buffer *buf = openTempWith("alpha\r\nbeta\r\n");

	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "DOS line endings"));
	TEST_ASSERT_NULL(strstr(E.statusmsg, "no final newline"));

	TEST_ASSERT_EQUAL_STRING("alpha", (char *)buf->row[0].chars);
	TEST_ASSERT_EQUAL_STRING("beta", (char *)buf->row[1].chars);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}

/* Both at once is the ordinary shape of a file off a Windows machine,
 * and the status line shows one message, so they combine. */
void test_open_reports_both_normalisations(void) {
	struct buffer *buf = openTempWith("alpha\r\nbeta");

	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "DOS line endings"));
	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "no final newline"));
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}

void test_open_well_formed_file_reports_nothing_extra(void) {
	struct buffer *buf = openTempWith("alpha\nbeta\n");

	TEST_ASSERT_NULL(strstr(E.statusmsg, "DOS"));
	TEST_ASSERT_NULL(strstr(E.statusmsg, "no final newline"));
	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "2 lines"));
	assertSerialises(buf, "alpha\nbeta\n");
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}

/* An empty file has no lines, so nothing to terminate and nothing to
 * report. */
void test_open_empty_file_reports_nothing_extra(void) {
	struct buffer *buf = openTempWith("");

	TEST_ASSERT_NULL(strstr(E.statusmsg, "no final newline"));
	TEST_ASSERT_EQUAL_INT(1, buf->numrows);
}

/* Piped input without a final newline used to leave the last row
 * non-empty, breaking the invariant every mutation path relies on
 * (see buffer.h).  editorOpen terminated unconditionally; this path
 * did not. */
void test_stdin_without_final_newline_keeps_invariant(void) {
	struct buffer *buf = loadStdinBuffer("alpha\nbeta", 10);
	TEST_ASSERT_NOT_NULL(buf);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("beta", (char *)buf->row[1].chars);
	TEST_ASSERT_EQUAL_INT(0, buf->row[buf->numrows - 1].size);
	destroyBuffer(buf);
}

void test_stdin_with_final_newline_unchanged(void) {
	struct buffer *buf = loadStdinBuffer("alpha\nbeta\n", 11);
	TEST_ASSERT_NOT_NULL(buf);
	TEST_ASSERT_EQUAL_INT(3, buf->numrows);
	TEST_ASSERT_EQUAL_INT(0, buf->row[buf->numrows - 1].size);
	destroyBuffer(buf);
}

/* A CR-only file has no '\n', so it arrives as one row with the CRs
 * kept, and save writes them back.  Reporting DOS would promise a
 * conversion that never happens; the missing newline is still real. */
void test_open_cr_only_file_is_not_reported_as_dos(void) {
	struct buffer *buf = openTempWith("alpha\rbeta\r");

	TEST_ASSERT_NULL(strstr(E.statusmsg, "DOS"));
	TEST_ASSERT_NOT_NULL(strstr(E.statusmsg, "no final newline"));

	/* One row of text, CR retained, plus the terminator row. */
	TEST_ASSERT_EQUAL_INT(2, buf->numrows);
	TEST_ASSERT_EQUAL_STRING("alpha\rbeta", (char *)buf->row[0].chars);
	TEST_ASSERT_EQUAL_INT(0, buf->dirty);
}


/* Write a scratch file and return a malloc'd path. */
static char *writeTempFile(const char *name, const char *contents) {
	char leaf[128];
	snprintf(leaf, sizeof(leaf), "emil_regress_%s_%d", name, (int)getpid());
	char *path = xmalloc(PATH_MAX);
	testTmpPath(path, PATH_MAX, leaf);
	FILE *fp = fopen(path, "w");
	if (fp) {
		fputs(contents, fp);
		fclose(fp);
	}
	return path;
}

/* B2: revert() on a buffer with no filename must not crash.
 *
 * revert() passed buf->filename straight to editorOpen, whose first
 * act is collapseHome(filename) -> path[0].  Starting emil with no
 * arguments leaves the initial buffer with filename == NULL, so
 * M-x revert-buffer segfaulted. */

void test_revert_null_filename_survives(void) {
	struct buffer *buf = startEditing();
	typeString("scratch text");
	TEST_ASSERT_NULL(buf->filename);

	metaX("revert-buffer"); /* must not dereference NULL */

	TEST_ASSERT(E.buf == buf);
	assertSerialises(E.buf, "scratch text\n");
	freeMinibuffer();
}

/* B4: revert() must refuse when the file no longer exists.
 *
 * editorOpen returns 0 both when it loaded a file and when the file is
 * missing (ENOENT -> posts "(New file)").  revert() only tested < 0,
 * so reverting a buffer whose file was never created silently replaced
 * it with an empty, clean buffer.  revert() ends in destroyBuffer(),
 * which frees the undo stack, so C-_ could not cross it and the work
 * was unrecoverable -- and because the replacement was clean, C-x C-c
 * exited without warning. */

void test_revert_missing_file_refuses(void) {
	char absent[PATH_MAX];
	testTmpPath(absent, sizeof(absent), "emil_regress_definitely_absent");
	unlink(absent);

	/* C-x C-f on a path with no file: a new, empty buffer for it. */
	struct buffer *scratch = startEditing();
	struct buffer *buf = findFileTyping(absent);
	TEST_ASSERT(buf != scratch);
	typeString("typed but never saved");
	TEST_ASSERT(buf->dirty != 0);

	metaX("revert-buffer");

	/* Buffer must be untouched: same object, same contents. */
	TEST_ASSERT(E.buf == buf);
	assertSerialises(E.buf, "typed but never saved\n");
	TEST_ASSERT(E.buf->dirty != 0);
	freeMinibuffer();
}

/* The case that must keep working: the file exists, the buffer has
 * unsaved edits, revert discards them and reloads from disk. */
void test_revert_existing_file_still_reloads(void) {
	char *path = writeTempFile("revert", "on disk line\n");

	struct buffer *scratch = startEditing();
	struct buffer *buf = findFileTyping(path);
	TEST_ASSERT(buf != scratch);
	buf->cx = 0;
	buf->cy = 0;
	typeString("UNSAVED EDIT ");
	assertSerialises(E.buf, "UNSAVED EDIT on disk line\n");

	metaX("revert-buffer");

	assertSerialises(E.buf, "on disk line\n");
	TEST_ASSERT_EQUAL_INT(0, E.buf->dirty);
	ASSERT_SAME_PATH(path, E.buf->filename);

	freeMinibuffer();
	unlink(path);
	free(path);
}



int main(void) {
	TEST_BEGIN();

	RUN_TEST(test_getline_short);
	RUN_TEST(test_getline_exact_120);
	RUN_TEST(test_getline_long);
	RUN_TEST(test_getline_no_newline);
	RUN_TEST(test_getline_empty);
	RUN_TEST(test_getline_multiple_reallocs);

	RUN_TEST(test_rows_to_string);
	RUN_TEST(test_open_directory_fails);
	RUN_TEST(test_open_trailing_slash_fails);

	RUN_TEST(test_utf8_valid_file);
	RUN_TEST(test_utf8_invalid_continuation);
	RUN_TEST(test_utf8_null_byte_rejected);
	RUN_TEST(test_utf8_truncated_multibyte);

	RUN_TEST(test_save_valid_utf8_succeeds);
	RUN_TEST(test_save_invalid_utf8_refused);

	/* Saving over a file the buffer knows nothing of */
	RUN_TEST(test_save_as_over_existing_file_declined_keeps_it);
	RUN_TEST(test_save_as_over_existing_file_accepted_writes);
	RUN_TEST(test_save_as_to_a_new_name_does_not_ask);
	RUN_TEST(test_first_save_of_unnamed_buffer_over_existing_file_asks);
	RUN_TEST(test_lf_name_existing);
	RUN_TEST(test_esc_name_existing);
	RUN_TEST(test_lf_name_new);
	RUN_TEST(test_esc_name_new);
	RUN_TEST(test_new_file_created_meanwhile_is_not_overwritten_silently);

	RUN_TEST(test_load_keeps_existing_final_newline);
	RUN_TEST(test_load_adds_missing_final_newline);
	RUN_TEST(test_load_adding_final_newline_leaves_buffer_clean);
	RUN_TEST(test_load_empty_file_serialises_to_nothing);

	RUN_TEST(test_open_reports_missing_final_newline);
	RUN_TEST(test_open_reports_dos_line_endings);
	RUN_TEST(test_open_reports_both_normalisations);
	RUN_TEST(test_open_cr_only_file_is_not_reported_as_dos);
	RUN_TEST(test_open_well_formed_file_reports_nothing_extra);
	RUN_TEST(test_open_empty_file_reports_nothing_extra);
	RUN_TEST(test_stdin_without_final_newline_keeps_invariant);
	RUN_TEST(test_stdin_with_final_newline_unchanged);


	RUN_TEST(test_revert_null_filename_survives);
	RUN_TEST(test_revert_missing_file_refuses);
	RUN_TEST(test_revert_existing_file_still_reloads);

	return TEST_END();
}
