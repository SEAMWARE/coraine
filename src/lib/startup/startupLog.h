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
#include <stdbool.h>                                      // bool



// -----------------------------------------------------------------------------
//
// startupLog - the log, -v / -d / the trace levels, and the libraries' log through it; after
// corArgsParse. logDir NULL: the log is stdout; else <logDir>/<progName>.log, and with logToStdout
// stdout too. Exits when the log cannot be set up.
//
extern void startupLog(const char* progName, const char* traceLevels, const char* logDir, bool logToStdout);

#endif  // STARTUP_STARTUPLOG_H_
