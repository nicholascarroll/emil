#ifndef EMIL_REGION_H
#define EMIL_REGION_H 1

#include "emil.h"
#include <stdint.h>

int markInvalid(void);
int markInvalidSilent(void);

void setMark(void);
void setMarkSilent(void);
void deactivateMark(void);
void popMark(void);
void clearMark(void);
void toggleRectangleMode(void);
void markBuffer(void);
void deleteRange(int startx, int starty, int endx, int endy,
		 int add_to_kill_ring);
void killRegion(void);
void copyRegion(void);
void yank(int count);
void yankPop(void);
void transformRange(int startx, int starty, int endx, int endy,
		    uint8_t *(*transformer)(uint8_t *));
void transformRegion(uint8_t *(*transformer)(uint8_t *));
void replaceRegex(void);
void stringRectangle(void);
void copyRectangle(void);
void killRectangle(void);
void yankRectangle(void);
void addToKillRing(const char *text, int is_rect, int rect_width,
		   int rect_height);
#endif
