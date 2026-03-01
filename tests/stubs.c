/* stubs.c — Stubs for the terminal I/O boundary.
 *
 * Strategy C: the test binary links every .o file except main.o and
 * terminal.o. This file provides:
 * - The global E and page_overlap that main.o normally defines.
 * - No-op replacements for terminal.o functions (the only functions
 *   that physically touch the terminal: read/write fd 0/1, termios,
 *   ioctl). */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "emil.h"

/* Normally defined in main.c */
struct editorConfig E;
const int page_overlap = 2;

void die(const char *s) {
	fprintf(stderr, "die: %s\n", s);
	abort();
}

void enableRawMode(void) {}
void disableRawMode(void) {}
void disableRawModeKeepScreen(void) {}

int editorReadKey(void) {
	return 0;
}

int getCursorPosition(int *rows, int *cols) {
	*rows = 24;
	*cols = 80;
	return 0;
}

int getWindowSize(int *rows, int *cols) {
	*rows = 24;
	*cols = 80;
	return 0;
}

void editorCopyToClipboard(const uint8_t *text) {
	(void)text;
}

void editorDeserializeUnicode(void) {}

void editorOpenShellDrawer(void) {}
