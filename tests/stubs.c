/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
/* stubs.c: Stubs for the terminal I/O boundary.
 *
 * The test binary links every .o file except main.o and terminal.o. 
 * This file provides:
 * - The global E and page_overlap that main.o normally defines.
 * - No-op replacements for terminal.o functions (the only functions
 *   that physically touch the terminal: read/write fd 0/1, termios,
 *   ioctl). */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "emil.h"

/* Normally defined in main.c */
struct config E;
const int page_overlap = 2;

void die(const char *s) {
	fprintf(stderr, "die: %s\n", s);
	abort();
}

void enableRawMode(void) {
}
void disableRawMode(void) {
}
void disableRawModeKeepScreen(void) {
}

/* Scripted key input.
 *
 * Modal loops (editorPrompt, zapToChar, getRegisterName) consume keys
 * through readKey().  A test drives them by filling test_key_script and
 * setting test_key_count; readKey() returns each in turn.  With no
 * script loaded the behaviour is the historical one (always 0), so
 * existing suites are unaffected. */
int test_key_script[64];
int test_key_count = 0;
int test_key_pos = 0;

int readKey(void) {
	if (test_key_pos < test_key_count)
		return test_key_script[test_key_pos++];
	return 0;
}

int getWindowSize(int *rows, int *cols) {
	*rows = 24;
	*cols = 80;
	return 0;
}

void copyToClipboard(const uint8_t *text) {
	(void)text;
}

void deserializeUnicode(void) {
}

void openShellDrawer(void) {
}
