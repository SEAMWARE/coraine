//
// FILE            mongocGlobals.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include "kargs/KArg.h"                              // KArg, _vp, KARGS_END

#include "currentState/mongoc/mongocGlobals.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// DB connection variables (owned by the plugin)
//
char*           mongocDbHost = "localhost";
char*           mongocDbName = "sw";
unsigned short  mongocDbPort = 27017;
char*           mongocDbUser = NULL;
char*           mongocDbPwd  = NULL;
char*           mongocDbURI     = NULL;
unsigned short  mongocDbTimeout = 30;

// Reserved database for global (non-tenant) state — JSON-LD context
// persistence (§ 5.13). Independent of --dbName so it survives tenant
// churn; configurable so parallel brokers sharing one mongo can each own a
// private global DB instead of colliding on the default.
char*           mongocGlobalDb  = "swBroker";



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
KArg mongocArgV[] =
{
  { "--dbHost", "-dbHost", KaString, _vp &mongocDbHost, KaOpt, _vp "localhost", NULL,  NULL,      "database server host"  },
  { "--dbName", "-dbName", KaString, _vp &mongocDbName, KaOpt, _vp "sw",  NULL,  NULL,      "database name"         },
  { "--globalDb", "-globalDb", KaString, _vp &mongocGlobalDb, KaOpt, _vp "swBroker", NULL, NULL, "reserved DB for global (non-tenant) state, e.g. JSON-LD contexts" },
  { "--dbPort", "-dbPort", KaUShort, _vp &mongocDbPort, KaOpt, _vp 27017,       _vp 1, _vp 65535, "database server port"  },
  { "--dbUser", "-dbUser", KaString, _vp &mongocDbUser, KaOpt, NULL,            NULL,  NULL,      "database user"         },
  { "--dbPwd",  "-dbPwd",  KaString, _vp &mongocDbPwd,  KaOpt, NULL,            NULL,  NULL,      "database password"     },
  { "--dbURI",     "-dbURI",     KaString, _vp &mongocDbURI,     KaOpt, NULL,      NULL,  NULL,      "full database URI"              },
  { "--dbTimeout", "-dbTimeout", KaUShort, _vp &mongocDbTimeout, KaOpt, _vp 30,    _vp 1, _vp 3600,  "database connection timeout (s)" },
  KARGS_END
};
