#ifndef MONGOC_MONGOCHAWATCH_H_
#define MONGOC_MONGOCHAWATCH_H_

//
// FILE            mongocHaWatch.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// The mongo end of the HA cache sync: a change stream, turned into HaEvents.
//
#include "ha/HaEvent.h"                                // HaApplyFunc



// -----------------------------------------------------------------------------
//
// mongocHaWatchStart - start watching for what the other broker instances write
//
// The DbDriver's haWatchStart. Verifies the deployment can do it at all, then
// leaves a thread of its own on the cursor. Never returns DB_OK on a mongod that
// cannot deliver a change stream — a sync that silently never fires is worse
// than a broker that refuses to start.
//
extern int mongocHaWatchStart(HaApplyFunc applyF);

#endif  // MONGOC_MONGOCHAWATCH_H_
