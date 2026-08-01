#ifndef EMIL_REGION_H
#define EMIL_REGION_H 1

#include "dbuf.h"
#include "emil.h"
#include <regex.h>
#include <stddef.h>
#include <stdint.h>

int markInvalid(void);
int markInvalidSilent(void);

void setMark(void);
void setMarkSilent(void);
void popMark(void);
void toggleRectangleMode(void);
void markBuffer(void);
void deleteRange(int startx, int starty, int endx, int endy,
		 int add_to_kill_ring);
void killRegion(void);
void copyRegion(void);
void yank(int uarg);
void yankPop(int uarg);
void transformRange(int startx, int starty, int endx, int endy,
		    uint8_t *(*transformer)(uint8_t *));
void transformRegion(uint8_t *(*transformer)(uint8_t *));
void replaceRegex(void);

/* Exposed for tests/test_replace.c; see region.c for contracts. */
const char *replacementTemplateError(const uint8_t *tmpl, size_t nsub);
int regexSubstituteAll(const regex_t *re, const uint8_t *subject, int len,
		       const uint8_t *tmpl, int notbol, int noteol,
		       struct dbuf *out, int *first_off, int *last_off);
void stringRectangle(void);
void stringRectangleWithText(uint8_t *string);
void copyRectangle(void);
void killRectangle(void);
void yankRectangle(void);
void addToKillRing(const char *text, int is_rect, int rect_width,
		   int rect_height);
#endif
