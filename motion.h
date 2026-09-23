/* Copyright (c) 2026 Nicholas Carroll. SPDX-License-Identifier: MIT */
#ifndef EMIL_MOTION_H
#define EMIL_MOTION_H 1

#include "emil.h"

/* Boundary detection */
int isParaBoundary(erow *row);

/* Cursor movement */
void moveCursor(int key, int count);

/* Boundary scanners.
 *
 * Each takes the buffer to scan and a position in it, and moves that
 * position -- in place -- to the boundary it looks for.  None reads
 * or moves point, and none consults E.buf: the position names the
 * start, the buffer names what it indexes.  Callers that want to move
 * point pass &buf->cx, &buf->cy.
 *
 * The word scanners used to take their start from point and treat the
 * arguments as outputs only, while their paragraph, sentence and sexp
 * siblings, with the same signature, started from the arguments.
 * Callers had to move point to ask about any other position. */

/* Word movement */
void forwardWordEnd(struct buffer *buf, int *dx, int *dy);
void backwardWordEnd(struct buffer *buf, int *dx, int *dy);
void forwardWord(int count);
void backWord(int count);

/* Paragraph movement */
void backwardParaBoundary(struct buffer *buf, int *cx, int *cy);
void forwardParaBoundary(struct buffer *buf, int *cx, int *cy);
void backPara(int count);
void forwardPara(int count);

/* Sexp (balanced expression) movement */
int bufferForwardSexpEnd(struct buffer *buf, int *cx, int *cy,
			 const char **errmsg);
void forwardSexp(int count);
void backwardSexp(int count);

/* Sentence movement */
int isSentenceBoundary(erow *row, int x);
int forwardSentenceEnd(struct buffer *buf, int *cx, int *cy);
int backwardSentenceStart(struct buffer *buf, int *cx, int *cy);
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
