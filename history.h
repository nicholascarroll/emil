#ifndef EMIL_HISTORY_H
#define EMIL_HISTORY_H

#include "emil.h"

void initHistory(struct history *hist);
void addHistory(struct history *hist, const char *str);
void addHistoryWithRect(struct history *hist, const char *str, int is_rectangle,
			int rect_width, int rect_height);
struct historyEntry *getHistoryAt(struct history *hist, int index);
void freeHistory(struct history *hist);
struct historyEntry *getLastHistory(struct history *hist);

#endif
