/*
 * Copyright (c) 2006, Ken McDonell.  All Rights Reserved.
 * Copyright (c) 2007, Aconex.  All Rights Reserved.
 * 
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or (at your
 * option) any later version.
 * 
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
 * for more details.
 */
#include "console.h"
#include <stdarg.h>
#include <pcp/pmapi.h>
#include <pcp/impl.h>

Console *console;

Console::Console(struct timeval origin) : QDialog()
{
    my.level = 0;
    if (pmDebug & DBG_TRACE_APPL0) {
	my.level |= App::DebugApp;		// general and UI tracing
	my.level |= App::DebugUi;
    }
    if (pmDebug & DBG_TRACE_APPL1)
	my.level |= App::DebugProtocol;	// trace time protocol
    if (pmDebug & DBG_TRACE_APPL2) {
	my.level |= App::DebugView;		// config files, for QA
	my.level |= App::DebugTimeless;
    }
    setupUi(this);

    my.origin = App::timevalToSeconds(origin);
    post("Console available");
}

void Console::post(const char *fmt, ...)
{
    static char buffer[4096];
    struct timeval now;
    va_list ap;
    int offset = 0;

    if (!(my.level & App::DebugApp))
	return;

    if (!(my.level & App::DebugTimeless)) {
	gettimeofday(&now, NULL);
	sprintf(buffer, "%6.2f: ", App::timevalToSeconds(now) - my.origin);
	offset = 8;
    }

    va_start(ap, fmt);
    vsnprintf(buffer+offset, sizeof(buffer)-offset, fmt, ap);
    va_end(ap);

    fputs(buffer, stderr);
    fputc('\n', stderr);
    text->append(QString(buffer));
}

bool Console::logLevel(int level)
{
    if (!(my.level & level))
	return false;
    return true;
}

void Console::post(int level, const char *fmt, ...)
{
    static char buffer[4096];
    struct timeval now;
    va_list ap;
    int offset = 0;

    if (!(my.level & level) && !(level & App::DebugForce))
	return;

    if (!(my.level & App::DebugTimeless)) {
	gettimeofday(&now, NULL);
	sprintf(buffer, "%6.2f: ", App::timevalToSeconds(now) - my.origin);
	offset = 8;
    }

    va_start(ap, fmt);
    vsnprintf(buffer+offset, sizeof(buffer)-offset, fmt, ap);
    va_end(ap);

    fputs(buffer, stderr);
    fputc('\n', stderr);
    text->append(QString(buffer));
}
