/* Copyright (c) 2021 chameleon, 2026 Nicholas Carroll.
 * SPDX-License-Identifier: MIT */
#ifndef EMIL_MESSAGE_H
#define EMIL_MESSAGE_H

void setStatusMessage(const char *fmt, ...)
	__attribute__((format(printf, 1, 2)));
void clearStatusMessage(void);

#endif /* EMIL_MESSAGE_H */
