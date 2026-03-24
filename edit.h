#ifndef EMIL_EDIT_H
#define EMIL_EDIT_H

#include "emil.h"

/* Character insertion */
void insertChar(struct buffer *bufr, int c, int count);
void insertUnicode(int count);

/* Line operations */
void insertNewlineRaw(void);
void insertNewline(int count);
void openLine(int count);
void insertNewlineAndIndent(int count);

/* Indentation */
void editorIndent(int rept);
void unindent(int rept);
void indentTabs(void);
void indentSpaces(void);

/* Character deletion */
void delChar(int count);
void backSpace(int count);

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
void forwardSexp(int count);
void backwardSexp(int count);

/* Sentence movement */
int forwardSentenceEnd(int *cx, int *cy);
int backwardSentenceStart(int *cx, int *cy);
void forwardSentence(int count);
void backwardSentence(int count);

/* Word transformations */
void wordTransform(int times, uint8_t *(*transformer)(uint8_t *));
void upcaseWord(int times);
void downcaseWord(int times);
void capitalCaseWord(int times);

/* Word deletion */
void deleteWord(int count);
void backspaceWord(int count);

/* Character/word transposition */
void transposeWords(void);
void transposeChars(void);

/* Line operations */
void killLine(int count);
void killLineBackwards(void);

/* Kill/mark operations */
void killSexp(int count);
void killParagraph(int count);
void markParagraph(void);

/* Sentence transposition */
void transposeSentences(void);

/* Zap to char */
void zapToChar(void);

/* Navigation */
void gotoLine(void);
void pageUp(int count);
void pageDown(int count);
void scrollLineUp(int count);
void scrollLineDown(int count);
void beginningOfLine(void);
void endOfLine(int count);
void quit(void);

/* External constants */
extern const int page_overlap;

#endif
