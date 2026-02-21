#ifndef EMIL_CLANG_H
#define EMIL_CLANG_H

#include "emil.h"

/* CTags: jump to definition of word at point (M-.) */
void editorCtagsJump(void);

/* CTags: pop back to previous location (M-,) */
void editorCtagsBack(void);

/* Toggle between .c and .h file (M-/) */
void editorToggleHeaderBody(void);

#endif /* EMIL_CLANG_H */
