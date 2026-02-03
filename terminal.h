#ifndef TERMINAL_H
#define TERMINAL_H

#include <stdint.h>

void die(const char *s);
void disableRawMode(void);
void enableRawMode(void);
int getCursorPosition(int *rows, int *cols);
int getWindowSize(int *rows, int *cols);
int editorReadKey(void);
void editorDeserializeUnicode(void);
void editorCopyToClipboard(const uint8_t *text);
void disableRawModeKeepScreen(void);
void editorOpenShellDrawer(void);

#endif /* TERMINAL_H */
