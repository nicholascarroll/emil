#ifndef EMIL_FIND_H
#define EMIL_FIND_H
#include "emil.h"
#include <stdint.h>
char *str_replace(char *orig, char *rep, char *with);
void findCallback(struct buffer *bufr, uint8_t *query, int key);
void editorFind(void);
void reverseFind(void);
void regexFind(void);
void regexFindWrapper(void);
void backwardRegexFind(void);
void backwardRegexFindWrapper(void);
uint8_t *transformerReplaceString(uint8_t *input);
void replaceString(void);
void queryReplace(void);
#endif
