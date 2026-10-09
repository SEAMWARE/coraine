//
// FILE            coraineFeatures.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stddef.h>                                  // NULL

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
  { "SCOPES",            COR_FEATURE_SCOPES            },
  { "DATASETID",         COR_FEATURE_DATASETID         },
  { "MULTI_TYPE",        COR_FEATURE_MULTI_TYPE        },
  { "CONTEXT_DL",        COR_FEATURE_CONTEXT_DL        },
  { "CONTEXT_HOSTING",   COR_FEATURE_CONTEXT_HOSTING   },
  { "TENANTS",           COR_FEATURE_TENANTS           },
  { "MONGOC",            COR_FEATURE_MONGOC            },
  { "ADMIN_API",         COR_FEATURE_ADMIN_API         },
  { "METRICS",           COR_FEATURE_METRICS           },
  { "TRANSPORTS",        COR_FEATURE_TRANSPORTS        },
  { "SERVICE_EXECUTION", COR_FEATURE_SERVICE_EXECUTION },
  { "SNAPSHOT_WRITE",    COR_FEATURE_SNAPSHOT_WRITE    },
  { "SNAPSHOT_SUBSCRIPTIONS", COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS },
  { "MIGRATE",           COR_FEATURE_MIGRATE           },
  { "HEALTH",            COR_FEATURE_HEALTH            },
  { "RESPONSE_BUDGET",   COR_FEATURE_RESPONSE_BUDGET   },
  { "AUTO_ENTITY_MAP",   COR_FEATURE_AUTO_ENTITY_MAP   },
  { "BRIDGE_RECORDS",    COR_FEATURE_BRIDGE_RECORDS    },
  { "CHANNEL_STATUS_IN", COR_FEATURE_CHANNEL_STATUS_IN },
  { "ICU_COLLATION",     COR_FEATURE_ICU_COLLATION     },
  { "LOCATION",          COR_FEATURE_LOCATION          },
  { "OBSERVATION_SPACE", COR_FEATURE_OBSERVATION_SPACE },
  { "OPERATION_SPACE",   COR_FEATURE_OPERATION_SPACE   },
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
  { "currentState", "corDB"     },
#if COR_FEATURE_MONGOC
  { "currentState", "mongoc"    },
#endif
  { "temporal",     "none"      },
  { "temporal",     "corDB"     },
  { "temporal",     "timescale" },
#if COR_FEATURE_ADMIN_API
  { "api",          "admin"     },
#endif
#if COR_FEATURE_TRANSPORTS
  { "transport",    "ws"        },
#endif
  { NULL,           NULL        }
};
