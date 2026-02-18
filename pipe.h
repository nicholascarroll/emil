#ifndef EMIL_PIPE_H
#define EMIL_PIPE_H
#include "emil.h"

uint8_t *editorPipe(struct editorConfig *ed, struct editorBuffer *buf);
void editorPipeCmd(struct editorConfig *ed, struct editorBuffer *bufr);
void editorDiffBufferWithFile(struct editorConfig *ed,
			      struct editorBuffer *bufr);

#endif
