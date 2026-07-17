#ifndef EMIL_PIPE_H
#define EMIL_PIPE_H
#include "emil.h"

uint8_t *editorPipe(int useRegion);
void pipeCmd(int useRegion);
void diffBufferWithFile(void);
uint8_t *pipeCommandCapture(const uint8_t *command, uint8_t *input);
uint8_t *pipeCommandCaptureIntr(const uint8_t *command, uint8_t *input,
				int intr_fd, int *out_canceled);

#endif
