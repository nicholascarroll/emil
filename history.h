#ifndef EMIL_HISTORY_H
#define EMIL_HISTORY_H

#include "emil.h"

void initHistory(struct editorHistory *hist);
void addHistory(struct editorHistory *hist, const char *str);
void addHistoryWithRect(struct editorHistory *hist, const char *str,
			int is_rectangle, int rect_width, int rect_height);
struct historyEntry *getHistoryAt(struct editorHistory *hist, int index);
void freeHistory(struct editorHistory *hist);
struct historyEntry *getLastHistory(struct editorHistory *hist);

#endif
