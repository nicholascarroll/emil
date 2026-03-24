#ifndef EMIL_BUFFER_H
#define EMIL_BUFFER_H
#include "emil.h"
void insertRow(struct buffer *bufr, int at, char *s, size_t len);
void freeRow(erow *row);
void delRow(struct buffer *bufr, int at);
void rowInsertChar(struct buffer *bufr, erow *row, int at, int c);
void rowInsertUnicode(struct buffer *bufr, erow *row, int at);
void rowAppendString(struct buffer *bufr, erow *row, char *s, size_t len);
void rowDelChar(struct buffer *bufr, erow *row, int at);
struct buffer *newBuffer(void);
void destroyBuffer(struct buffer *buf);
void updateBuffer(struct buffer *buf);
void switchToNamedBuffer(void);
void nextBuffer(void);
void previousBuffer(void);
void killBuffer(void);
void computeDisplayNames(void);
void invalidateScreenCache(struct buffer *buf);
void buildScreenCache(struct buffer *buf, int screencols);
int getScreenLineForRow(struct buffer *buf, int row, int screencols);
int calculateLineWidth(erow *row);
int charsToDisplayColumn(erow *row, int char_pos);
int countScreenLines(erow *row, int screencols);
int wordWrapBreak(erow *row, int screencols, int line_start_col,
		  int line_start_byte, int *break_col, int *break_byte);
void cursorScreenLine(erow *row, int cursor_col, int screencols, int *out_line,
		      int *out_col);
int sublineBounds(erow *row, int screencols, int target_subline,
		  int *start_byte, int *end_byte);
int displayColumnToByteOffset(erow *row, int screencols, int target_subline,
			      int target_col);

void clampPositions(struct buffer *buf);

struct buffer *findBufferByName(const char *name);
struct buffer *findOrCreateSpecialBuffer(const char *name);
void clearBuffer(struct buffer *buf);
void closeSpecialBuffer(const char *name);

#endif
