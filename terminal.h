/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_TERMINAL_H
#define EMIL_TERMINAL_H

#include <stdint.h>
#include <stddef.h>

void die(const char *s);
void disableRawMode(void);
void enableRawMode(void);
void applyRawMode(void);
int rawModeDivergence(char *buf, size_t n);
void getWindowSize(int *rows, int *cols);
int readKey(void);
void deserializeUnicode(void);
void copyToClipboard(const uint8_t *text);
void disableRawModeKeepScreen(void);
#ifndef __wasi__ /* no job control; see dispatchMisc() in keymap.c */
void openShellDrawer(void);
#endif
void installHandler(int signum, void (*handler)(int), int flags);

#endif /* EMIL_TERMINAL_H */
