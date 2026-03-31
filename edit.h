#ifndef EMIL_EDIT_H
#define EMIL_EDIT_H

#include "emil.h"
#include "motion.h"

/* Character insertion */
void insertChar(struct buffer *bufr, int c, int count);
void insertUnicode(int count);

/* Line operations */
void splitLineAtPoint(void);
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

/* Quit */
void quit(void);

#endif
