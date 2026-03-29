#include "message.h"
#include "emil.h"
#include <stdarg.h>
#include <stdio.h>

extern struct config E;

void setStatusMessage(const char *fmt, ...) {
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(E.statusmsg, sizeof(E.statusmsg), fmt, ap);
	va_end(ap);
	E.statusmsg_show = 1;
}
