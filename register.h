#ifndef REGISTER_H
#define REGISTER_H
#include "emil.h"

void editorJumpToRegister(struct editorConfig *ed);
void editorPointToRegister(struct editorConfig *ed);
void editorRegionToRegister(struct editorConfig *ed, struct editorBuffer *bufr);
void editorRectToRegister(struct editorConfig *ed, struct editorBuffer *bufr);
void editorIncrementRegister(struct editorConfig *ed,
			     struct editorBuffer *bufr);
void editorInsertRegister(struct editorConfig *ed, struct editorBuffer *bufr);
void editorViewRegister(struct editorConfig *ed, struct editorBuffer *bufr);
#endif
