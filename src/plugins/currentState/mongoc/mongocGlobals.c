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



// -----------------------------------------------------------------------------
//
// mongocArgV - plugin-contributed CLI args
//
KArg mongocArgV[] =
{
  { "--dbHost", "-dbHost", KaString, _vp &mongocDbHost, KaOpt, _vp "localhost", NULL,  NULL,      "database server host"  },
  { "--dbName", "-dbName", KaString, _vp &mongocDbName, KaOpt, _vp "sw",  NULL,  NULL,      "database name"         },
  { "--dbPort", "-dbPort", KaUShort, _vp &mongocDbPort, KaOpt, _vp 27017,       _vp 1, _vp 65535, "database server port"  },
  { "--dbUser", "-dbUser", KaString, _vp &mongocDbUser, KaOpt, NULL,            NULL,  NULL,      "database user"         },
  { "--dbPwd",  "-dbPwd",  KaString, _vp &mongocDbPwd,  KaOpt, NULL,            NULL,  NULL,      "database password"     },
  { "--dbURI",     "-dbURI",     KaString, _vp &mongocDbURI,     KaOpt, NULL,      NULL,  NULL,      "full database URI"              },
  { "--dbTimeout", "-dbTimeout", KaUShort, _vp &mongocDbTimeout, KaOpt, _vp 30,    _vp 1, _vp 3600,  "database connection timeout (s)" },
  KARGS_END
};
