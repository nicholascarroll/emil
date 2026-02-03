#include "message.h"
#include "emil.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

extern struct editorConfig E;

void editorSetStatusMessage(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
    va_end(ap);
    E.statusmsg_time = time(NULL);
}