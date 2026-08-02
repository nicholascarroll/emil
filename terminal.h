/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_TERMINAL_H
#define EMIL_TERMINAL_H

#include <stdint.h>

void die(const char *s);
void disableRawMode(void);
void enableRawMode(void);
void applyRawMode(void);
void getWindowSize(int *rows, int *cols);
int readKey(void);
void deserializeUnicode(void);
void copyToClipboard(const uint8_t *text);
void disableRawModeKeepScreen(void);
void openShellDrawer(void);
void installHandler(int signum, void (*handler)(int), int flags);

#endif /* EMIL_TERMINAL_H */
