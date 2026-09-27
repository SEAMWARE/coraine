//
// FILE            mongocGlobals.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include "corArgs/CorArg.h"                          // CorArg, _vp, CORARGS_END

#include "currentState/mongoc/mongocGlobals.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// DB connection variables (owned by the plugin)
//
char*           mongocDbHost = "localhost";
char*           mongocDbName = "cor";
unsigned short  mongocDbPort = 27017;
char*           mongocDbUser = NULL;
char*           mongocDbPwd  = NULL;
char*           mongocDbURI     = NULL;
unsigned short  mongocDbTimeout = 30;

// Reserved database for global (non-tenant) state — JSON-LD context
// persistence (§ 5.13). Independent of --dbName so it survives tenant
// churn; configurable so parallel brokers sharing one mongo can each own a
// private global DB instead of colliding on the default.
char*           mongocGlobalDb  = "coraine";



// -----------------------------------------------------------------------------
//
// mongocUriString - the connection URI, as built by mongocInit
//
// Kept so a client can be created outside the pool: the HA watch holds one for
// the lifetime of the broker, and a pool slot taken away from the request
// threads forever would be a poor trade for a thread that spends its life
// blocked on a cursor.
//
char            mongocUriString[512] = { 0 };



// -----------------------------------------------------------------------------
//
// mongocArgV - plugin-contributed CLI args
//
CorArg mongocArgV[] =
{
  { "--dbHost", "-dbHost", CorArgString, _vp &mongocDbHost, CorArgOpt, _vp "localhost", NULL,  NULL,      "database server host"  },
  { "--dbName", "-dbName", CorArgString, _vp &mongocDbName, CorArgOpt, _vp "cor",  NULL,  NULL,      "database name"         },
  { "--globalDb", "-globalDb", CorArgString, _vp &mongocGlobalDb, CorArgOpt, _vp "coraine", NULL, NULL, "reserved DB for global (non-tenant) state, e.g. JSON-LD contexts" },
  { "--dbPort", "-dbPort", CorArgUShort, _vp &mongocDbPort, CorArgOpt, _vp 27017,   _vp 1, _vp 65535, "database server port"  },
  { "--dbUser", "-dbUser", CorArgString, _vp &mongocDbUser, CorArgOpt, NULL,    NULL,  NULL,      "database user"         },
  { "--dbPwd",  "-dbPwd",  CorArgString, _vp &mongocDbPwd,  CorArgOpt, NULL,        NULL,  NULL,      "database password"     },
  { "--dbURI",     "-dbURI",     CorArgString, _vp &mongocDbURI, CorArgOpt, NULL,  NULL,  NULL,      "full database URI"              },
  { "--dbTimeout", "-dbTimeout", CorArgUShort, _vp &mongocDbTimeout, CorArgOpt, _vp 30,    _vp 1, _vp 3600,  "database connection timeout (s)" },
  CORARGS_END
};
