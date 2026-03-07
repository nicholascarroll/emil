#include <stdlib.h>
#include <string.h>
#include "emil.h"
#include "history.h"
#include "util.h"

extern struct editorConfig E;

void initHistory(struct editorHistory *hist) {
	hist->head = NULL;
	hist->tail = NULL;
	hist->count = 0;
}

void addHistory(struct editorHistory *hist, const char *str) {
	addHistoryWithRect(hist, str, 0, 0, 0);
}

void addHistoryWithRect(struct editorHistory *hist, const char *str,
			int is_rectangle, int rect_width, int rect_height) {
	if (!str || strlen(str) == 0) {
		return;
	}

	/* Don't add duplicates of the most recent entry */
	if (hist->tail && strcmp(hist->tail->str, str) == 0 &&
	    hist->tail->is_rectangle == is_rectangle &&
	    hist->tail->rect_width == rect_width &&
	    hist->tail->rect_height == rect_height) {
		return;
	}

	/* Create new entry */
	struct historyEntry *entry = xmalloc(sizeof(struct historyEntry));
	entry->str = xstrdup(str);
	entry->is_rectangle = is_rectangle;
	entry->rect_width = rect_width;
	entry->rect_height = rect_height;
	entry->next = NULL;
	entry->prev = hist->tail;

	/* Add to list */
	if (hist->tail) {
		hist->tail->next = entry;
	} else {
		hist->head = entry;
	}
	hist->tail = entry;
	hist->count++;

	/* Remove oldest entries if we exceed the limit */
	while (hist->count > HISTORY_MAX_ENTRIES) {
		struct historyEntry *old = hist->head;
		hist->head = old->next;
		if (hist->head) {
			hist->head->prev = NULL;
		}
		free(old->str);
		free(old);
		hist->count--;
	}
}

struct historyEntry *getHistoryAt(struct editorHistory *hist, int index) {
	if (index < 0 || index >= hist->count) {
		return NULL;
	}

	struct historyEntry *entry = hist->tail;
	for (int i = hist->count - 1; i > index && entry; i--) {
		entry = entry->prev;
	}

	return entry;
}

void freeHistory(struct editorHistory *hist) {
	struct historyEntry *entry = hist->head;
	while (entry) {
		struct historyEntry *next = entry->next;
		free(entry->str);
		free(entry);
		entry = next;
	}
	initHistory(hist);
}

struct historyEntry *getLastHistory(struct editorHistory *hist) {
	return hist->tail;
}
