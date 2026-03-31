#ifndef EMIL_MOTION_H
#define EMIL_MOTION_H 1

#include "emil.h"

/* Boundary detection */
int isParaBoundary(erow *row);

/* Cursor movement */
void moveCursor(int key, int count);

/* Word movement */
void forwardWordEnd(int *dx, int *dy);
void backwardWordEnd(int *dx, int *dy);
void forwardWord(int count);
void backWord(int count);

/* Paragraph movement */
void backwardParaBoundary(int *cx, int *cy);
void forwardParaBoundary(int *cx, int *cy);
void backPara(int count);
void forwardPara(int count);

/* Sexp (balanced expression) movement */
int bufferForwardSexpEnd(int *cx, int *cy, const char **errmsg);
void forwardSexp(int count);
void backwardSexp(int count);

/* Sentence movement */
int isSentenceBoundary(erow *row, int x);
int forwardSentenceEnd(int *cx, int *cy);
int backwardSentenceStart(int *cx, int *cy);
void forwardSentence(int count);
void backwardSentence(int count);

/* Navigation */
void pageUp(int count);
void pageDown(int count);
void scrollLineUp(int count);
void scrollLineDown(int count);
void beginningOfLine(void);
void endOfLine(int count);
void gotoLine(void);

/* External constants */
extern const int page_overlap;

#endif
