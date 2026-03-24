#ifndef EMIL_TRANSFORM_H
#define EMIL_TRANSFORM_H
#include <stdint.h>

struct config;
struct buffer;

uint8_t *transformerUpcase(uint8_t *);
uint8_t *transformerDowncase(uint8_t *);
uint8_t *transformerCapitalCase(uint8_t *);
uint8_t *transformerTransposeWords(uint8_t *);
uint8_t *transformerTransposeChars(uint8_t *);
void capitalizeRegion(void);
#endif
