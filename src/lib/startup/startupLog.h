#ifndef STARTUP_STARTUPLOG_H_
#define STARTUP_STARTUPLOG_H_

//
// FILE            startupLog.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//



// -----------------------------------------------------------------------------
//
// startupLog - the log (to the screen - stdout), -v / -d / the trace levels, and the libraries' log
// through it; after corArgsParse. Exits when the log cannot be set up.
//
extern void startupLog(const char* progName, const char* traceLevels);

#endif  // STARTUP_STARTUPLOG_H_
