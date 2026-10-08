//
// FILE            startupLog.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                       // NULL

#include "corLog/corLog.h"                                // corLogInit, COR_X, COR_W
#include "corLog/corLogGlobals.h"                         // corLogInfo, corLogVerbose, corLogDebug, corLogTraceLevels
#include "corLog/corLogOut.h"                             // corLogOut
#include "corBase/corBaseInit.h"                          // corBaseInit, corBaseTraceLevelsSet
#include "corArgs/corArgs.h"                              // corArgsBuiltinVerbose, corArgsBuiltinDebug

#include "startup/startupLog.h"                           // Own interface



// -----------------------------------------------------------------------------
//
// startupLog -
//
void startupLog(const char* progName, const char* traceLevels)
{
  if (corLogInit(progName, NULL, true, NULL, traceLevels, corArgsBuiltinVerbose, corArgsBuiltinDebug, false) != 0)
    COR_X(1, "corLogInit failed");

#ifndef COR_T_ON
  //
  // A release build: its traces are compiled away (corLog.h), so a trace level turns on nothing -
  // said once, rather than leaving an operator to wonder why the log stays quiet.
  //
  if ((traceLevels != NULL) && (traceLevels[0] != 0))
    COR_W("--traceLevels %s: this broker is a release build, its traces are compiled out - nothing to turn on", traceLevels);
#endif

  //
  // The libraries log through corBase's callback (COR_LIB_*), and until it is set
  // they have no log to write to - their errors go to stderr, the rest nowhere.
  // corLogOut has the callback's signature, so their lines land in OUR log file,
  // with their own file, line and function, gated by the same -v/-t switches.
  //
  corBaseInit(corLogOut);

  //
  // ... and a library trace that is off is decided inline, on our own bitmask, instead of
  // costing a call into corLogOut per trace line - most of them in per-node code.
  //
  corBaseTraceLevelsSet(corLogTraceLevels, sizeof(corLogTraceLevels) / sizeof(corLogTraceLevels[0]));

  //
  // Each switch steers its OWN class of output: -v drives COR_V, -d drives COR_D,
  // and a trace level drives COR_T for that level. Nothing else.
  //
  // corLogInit does not do that. It derives corLogInfo/corLogVerbose/corLogDebug from a single
  // CUMULATIVE level (CERO 0, ERR 1, WARN 2, INFO 3, VERBOSE 4, TRACE 5,
  // DEBUG 6), and it sets that level to 5 as soon as ANY trace level is asked
  // for - so `-t 235`, which asks for one line about one decision, silently
  // turns on every COR_I and COR_V in the broker as well. That is how the admin
  // test came to report three fields changed when one option was passed.
  //
  // We do not pass a logLevel at all (the NULL above), so without the bump the
  // level would stay -1 and both would be off. Restoring that here is therefore
  // not a policy of our own; it is what corLogInit computes for our own arguments,
  // minus a bump we never asked for.
  //
  // Fixing it in corLog is the right place and NOT today's errand: the library
  // is shared with consumers that pass a real logLevel and have a large user
  // base, and there the same line silently DOWNGRADES an explicit
  // `--logLevel DEBUG` to 5 and takes COR_D away. One thing at a time.
  //
  // ⚠️ corLogInfo follows -v because there is no -i: kargs has corArgsBuiltinVerbose and
  // corArgsBuiltinDebug and no info switch, and INFO sits below VERBOSE on that same
  // ladder. Give it its own option and this becomes that option.
  //
  corLogInfo    = corArgsBuiltinVerbose;
  corLogVerbose = corArgsBuiltinVerbose;
  corLogDebug   = corArgsBuiltinDebug;
}
