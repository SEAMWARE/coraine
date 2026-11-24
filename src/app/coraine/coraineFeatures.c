//
// FILE            coraineFeatures.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                  // NULL
#include <string.h>                                  // strcmp

#include "coraineFeatures.h"                      // Own interface



// -----------------------------------------------------------------------------
//
// coraineFeatures -
//
// Plain initialisers rather than #ifdef per line: CMakeLists.txt defines every
// COR_FEATURE_* to 1 or 0 (never leaves one undefined), so each flag IS a C
// value here. -Wundef turns a misspelt name into a compile error rather than a
// feature that silently reports itself off.
//
// The order is the order of the options in CMakeLists.txt, whose COR_FEATURES
// list is the other half of this pair. Adding a feature there and forgetting it
// here costs a line of reporting; REMOVING one there and leaving it here is a
// compile error, which is the direction that matters.
//
const CoraineFeature coraineFeatures[] =
{
  { "SUBSCRIPTIONS",     COR_FEATURE_SUBSCRIPTIONS     },
  { "REGISTRATIONS",     COR_FEATURE_REGISTRATIONS     },
  { "GEOQ",              COR_FEATURE_GEOQ              },
  { "CONTEXT_HOSTING",   COR_FEATURE_CONTEXT_HOSTING   },
  { "MONGOC",            COR_FEATURE_MONGOC            },
  { "CORDB",             COR_FEATURE_CORDB             },
  { "ADMIN_API",         COR_FEATURE_ADMIN_API         },
  { "METRICS",           COR_FEATURE_METRICS           },
  { "AUTH",              COR_FEATURE_AUTH              },
  { "POLICY_API",        COR_FEATURE_POLICY_API        },
  { "COR_PROTOCOL",      COR_FEATURE_COR_PROTOCOL      },
  { "TRANSPORTS",        COR_FEATURE_TRANSPORTS        },
  { "HA_RUNS",           COR_FEATURE_HA_RUNS           },
  { "SSE",               COR_FEATURE_SSE               },
  { "SERVICE_EXECUTION", COR_FEATURE_SERVICE_EXECUTION },
  { "SNAPSHOT_WRITE",    COR_FEATURE_SNAPSHOT_WRITE    },
  { "SNAPSHOT_SUBSCRIPTIONS", COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS },
  { "MIGRATE",           COR_FEATURE_MIGRATE           },
  { "HEALTH",            COR_FEATURE_HEALTH            },
  { "RESPONSE_BUDGET",   COR_FEATURE_RESPONSE_BUDGET   },
  { "AUTO_ENTITY_MAP",   COR_FEATURE_AUTO_ENTITY_MAP   },
  { "CHANNEL_STATUS_IN", COR_FEATURE_CHANNEL_STATUS_IN },
  { "CHANNEL_CHECKS",    COR_FEATURE_CHANNEL_CHECKS    },
  { "CHANNEL_WRITE_BACK", COR_FEATURE_CHANNEL_WRITE_BACK },
  { "BRIDGE_INVENTORY",  COR_FEATURE_BRIDGE_INVENTORY  },
  { "BRIDGE_HTTP_LISTEN", COR_FEATURE_BRIDGE_HTTP_LISTEN },
  { "ADMIN_CONFIG",      COR_FEATURE_ADMIN_CONFIG      },
  { "DATA_CHANNELS",     COR_FEATURE_DATA_CHANNELS     },
  { "ASSET_CHANNELS",    COR_FEATURE_ASSET_CHANNELS    },
  { "ASSET_CHANNEL_SERVICES", COR_FEATURE_ASSET_CHANNEL_SERVICES },
  { "ICU_COLLATION",     COR_FEATURE_ICU_COLLATION     },
  { "TIMEPOINT",         COR_FEATURE_TIMEPOINT         },
  { "TEMPORAL_ENTITY_MAP", COR_FEATURE_TEMPORAL_ENTITY_MAP },
  { "AUTH_OIDC",         COR_FEATURE_AUTH_OIDC         },
  { "AUTH_METADATA",     COR_FEATURE_AUTH_METADATA     },
  { "Q_PREFILTER",       COR_FEATURE_Q_PREFILTER       },
  { "DB_INDEXES",        COR_FEATURE_DB_INDEXES        },
  { "PROBES",            COR_FEATURE_PROBES            },
  { "LOG_LINES",         COR_FEATURE_LOG_LINES         },
  { "TROE_CORDB",        COR_FEATURE_TROE_CORDB        },
  { "TROE_TIMESCALE",    COR_FEATURE_TROE_TIMESCALE    },
  { NULL,                false                         }
};



// -----------------------------------------------------------------------------
//
// coraineBuiltPlugins -
//
// Mirrors the ADD_SUBDIRECTORY list in CMakeLists.txt. The two that are gated by
// a feature are gated by the same 0/1 define the feature list uses, so they
// cannot claim to exist in a build that did not compile them.
//
const CorainePlugin coraineBuiltPlugins[] =
{
#if COR_FEATURE_CORDB
  { "currentState", "corDB"     },
#endif
#if COR_FEATURE_MONGOC
  { "currentState", "mongoc"    },
#endif
  { "temporal",     "none"      },
#if COR_FEATURE_TROE_CORDB
  { "temporal",     "corDB"     },
#endif
#if COR_FEATURE_TROE_TIMESCALE
  { "temporal",     "timescale" },
#endif
#if COR_FEATURE_ADMIN_API
  { "api",          "admin"     },
#endif
#if COR_FEATURE_TRANSPORTS
  { "transport",    "ws"        },
#endif
  { NULL,           NULL        }
};



// -----------------------------------------------------------------------------
//
// coraineHttpServers -
//
const char* const coraineHttpServers[] =
{
#if COR_HTTP_SERVER_MHD
  "mhd",
#endif
#if COR_HTTP_SERVER_BUILTIN
  "builtin",
#endif
  NULL
};



char* coraineHttpServer = NULL;   // set by corArgsParse - its default is coraineHttpServers[0]



// -----------------------------------------------------------------------------
//
// coraineHttpServerBuilt -
//
bool coraineHttpServerBuilt(const char* name)
{
  for (int ix = 0; coraineHttpServers[ix] != NULL; ix++)
  {
    if (strcmp(name, coraineHttpServers[ix]) == 0)
      return true;
  }

  return false;
}
