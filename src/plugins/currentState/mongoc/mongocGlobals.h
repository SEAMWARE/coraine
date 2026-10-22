#ifndef MONGOC_MONGOCGLOBALS_H_
#define MONGOC_MONGOCGLOBALS_H_

//
// FILE            mongocGlobals.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corArgs/CorArg.h"                          // CorArg



// -----------------------------------------------------------------------------
//
// DB connection variables (owned by the plugin)
//
extern char*           mongocDbHost;
extern char*           mongocDbName;
extern unsigned short  mongocDbPort;
extern char*           mongocDbUser;
extern char*           mongocDbPwd;
extern char*           mongocDbURI;
extern unsigned short  mongocDbTimeout;
#if COR_FEATURE_DB_INDEXES
extern char*           mongocDbIndexes;
#endif
extern char*           mongocGlobalDb;
extern char            mongocUriString[512];   // the connection URI as built by mongocInit



// -----------------------------------------------------------------------------
//
// mongocArgV - plugin-contributed CLI args
//
extern CorArg mongocArgV[];

#endif  // MONGOC_MONGOCGLOBALS_H_
