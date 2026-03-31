/* test_keymap.c — minimal characterisation tests for the keymap.
 *
 * Only pins behaviours later phases could silently break:
 *
 *   1. C-x C-s routes to CMD_SAVE (one assertion exercises the
 *      prefix state machine held as `static` inside resolveBinding).
 *   2. Universal argument: C-u 4 2 results in E.uarg == 42 before the
 *      next command dispatches, and E.uarg resets to 0 after dispatch.
 *   3. E.micro redo micro-state: after CMD_REDO, a following CMD_UNDO
 *      invokes doRedo, not doUndo.  This is the Emacs convention that
 *      C-_ immediately after C-x C-_ continues redoing rather than
 *      flipping direction back into the undo history.
 *
 * The commands themselves remain indirectly tested through test_edit
 * and friends; this file is deliberately narrow. */

#include "test.h"
#include "test_harness.h"
#include "emil.h"
#include "keymap.h"
#include "mutate.h"
#include "undo.h"
#include <string.h>

extern struct config E;

void setUp(void) {
	initTestEditor();
}
void tearDown(void) {
	cleanupTestEditor();
}

/* --- 1. C-x C-s → CMD_SAVE ---------------------------------------- */

void test_ctrl_x_ctrl_s_resolves_to_save(void) {
	/* First key: C-x.  The prefix state machine enters PREFIX_CTRL_X
	 * and returns CMD_NONE (waiting for the next key). */
	int cmd1 = resolveBinding(CTRL('x'));
	TEST_ASSERT_EQUAL_INT(CMD_NONE, cmd1);

	/* Second key: C-s.  With the prefix armed, this resolves to
	 * CMD_SAVE (not CMD_ISEARCH, which is the unprefixed binding). */
	int cmd2 = resolveBinding(CTRL('s'));
	TEST_ASSERT_EQUAL_INT(CMD_SAVE, cmd2);

	/* Sanity: after dispatch the prefix has cleared — a fresh C-s
	 * now resolves to CMD_ISEARCH. */
	int cmd3 = resolveBinding(CTRL('s'));
	TEST_ASSERT_EQUAL_INT(CMD_ISEARCH, cmd3);
}

/* --- 2. Universal argument: C-u 2 3 → uarg == 23, resets after ----- */

void test_universal_arg_accumulates_then_resets(void) {
	struct buffer *buf = make_test_buffer("abc");
	(void)buf;

	/* C-u: resolveBinding returns CMD_UNIVERSAL_ARG.  processKeypress
	 * acts on it — sets E.uarg = 4 and returns without clearing it. */
	int cmd = resolveBinding(CTRL('u'));
	TEST_ASSERT_EQUAL_INT(CMD_UNIVERSAL_ARG, cmd);
	processKeypress(cmd);
	TEST_ASSERT_EQUAL_INT(4, E.uarg);

	/* Digit '2': with uarg armed and uarg == 4, the first digit
	 * REPLACES (not accumulates) — the default-4 seed is discarded.
	 * resolveBinding handles this in its printable-char branch and
	 * returns CMD_NONE. */
	cmd = resolveBinding('2');
	TEST_ASSERT_EQUAL_INT(CMD_NONE, cmd);
	/* uarg mutated in resolveBinding; the real main loop skips
	 * dispatch when cmd == CMD_NONE, so we don't call processKeypress. */
	TEST_ASSERT_EQUAL_INT(2, E.uarg);

	/* Digit '3': with uarg != 4 now, accumulates.  2 * 10 + 3 = 23. */
	cmd = resolveBinding('3');
	TEST_ASSERT_EQUAL_INT(CMD_NONE, cmd);
	TEST_ASSERT_EQUAL_INT(23, E.uarg);

	/* Before the next real command dispatches, uarg is still 23 —
	 * the prefix is read once by processKeypress and zeroed at the
	 * bottom of dispatch.  Simulate a following command that reads
	 * uarg: forward-char (CMD_FORWARD_CHAR).  After dispatch, uarg
	 * is 0. */
	processKeypress(CMD_FORWARD_CHAR);
	TEST_ASSERT_EQUAL_INT(0, E.uarg);
}

/* --- 3. E.micro: CMD_REDO → following CMD_UNDO invokes doRedo ------ */

void test_redo_micro_reroutes_following_undo_to_redo(void) {
	/* Set up a buffer with an undo record so doUndo has something
	 * to pop.  Use mutateInsert, which records undo properly. */
	struct buffer *buf = make_test_buffer("hello");
	buf->cx = 5;
	buf->cy = 0;
	mutateInsert(buf, buf->cx, buf->cy, (const uint8_t *)"!", 1, NULL,
		     NULL);
	TEST_ASSERT_EQUAL_STRING("hello!", row_str(buf, 0));
	TEST_ASSERT_NOT_NULL(buf->undo);

	/* Undo: now redo stack has one record. */
	doUndo(buf, 1);
	TEST_ASSERT_NOT_NULL(buf->redo);
	TEST_ASSERT_EQUAL_STRING("hello", row_str(buf, 0));

	/* Arm E.micro = CMD_REDO.  In normal flow, dispatchBuffer does
	 * this after a CMD_REDO step when more redo remains. */
	E.micro = CMD_REDO;

	/* Now dispatch CMD_UNDO.  With E.micro == CMD_REDO, processKeypress
	 * routes to doRedo, not doUndo.  The observable consequence: the
	 * buffer content returns to the post-insert state ("hello!"),
	 * which is what doRedo would do; doUndo on an empty undo stack
	 * would do nothing (or re-undo, depending on state). */
	processKeypress(CMD_UNDO);
	TEST_ASSERT_EQUAL_STRING("hello!", row_str(buf, 0));
}

/* --- runner ------------------------------------------------------- */

int main(void) {
	TEST_BEGIN();
	RUN_TEST(test_ctrl_x_ctrl_s_resolves_to_save);
	RUN_TEST(test_universal_arg_accumulates_then_resets);
	RUN_TEST(test_redo_micro_reroutes_following_undo_to_redo);
	return TEST_END();
}
