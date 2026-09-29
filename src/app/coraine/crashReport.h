#ifndef SRC_APP_CORAINE_CRASHREPORT_H_
#define SRC_APP_CORAINE_CRASHREPORT_H_

//
// FILE            crashReport.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//



// -----------------------------------------------------------------------------
//
// crashReportInstall - on a fatal signal, report what is needed to REPRODUCE the crash
//
// SIGSEGV, SIGBUS, SIGFPE, SIGILL and SIGABRT. The report goes to the log (file or stdout) AND stderr:
//   - the signal, and the broker's version
//   - the command line: the configuration it was started with
//   - the request the crashing thread was handling: verb, path + query, headers (credentials
//     left out) and the payload
//   - the stack
// Then the signal is raised again with its default action, so the exit status (and a core
// dump, where enabled) is the signal's own.
//
// Call once, early in main, with main's argc/argv (kept by pointer - they live as long as main).
//
extern void crashReportInstall(int argC, char** argV);

#endif  // SRC_APP_CORAINE_CRASHREPORT_H_
