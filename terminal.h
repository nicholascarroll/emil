#ifndef EMIL_TERMINAL_H
#define EMIL_TERMINAL_H

#include <stdint.h>

void die(const char *s);
void disableRawMode(void);
void enableRawMode(void);
int getCursorPosition(int *rows, int *cols);
int getWindowSize(int *rows, int *cols);
int readKey(void);
void deserializeUnicode(void);
void copyToClipboard(const uint8_t *text);
void disableRawModeKeepScreen(void);
void openShellDrawer(void);

#endif /* EMIL_TERMINAL_H */
