//
// FILE            mongocInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//

#include <stdio.h>                                   // snprintf
#include <string.h>                                  // strdup, strlen, strncmp, strcmp

#include <mongoc/mongoc.h>                           // mongoc_init, mongoc_client_pool_new, ...

#include "corLog/corLog.h"                               // COR_I, COR_E

#include "db/Tenant.h"                               // tenantInit, tenantGetOrCreate, tenant0
#include "shared/geoMatch.h"                                      // geoMatchInit
#include "currentState/mongoc/mongocGeoIndex.h"                   // mongocGeoIndexInit
#include "currentState/mongoc/mongocGlobals.h"                    // mongocDbHost, mongocDbName, ...
#include "currentState/mongoc/mongocStorageFormat.h"              // mongocStorageFormat
#include "currentState/mongoc/mongocTenantSetup.h"                // mongocTenantSetup
#include "currentState/mongoc/mongocVersion.h"                    // mongocServerVersionGet
#include "currentState/mongoc/mongocInit.h"                       // Own interface



// -----------------------------------------------------------------------------
//
// Shared state - accessed by other mongoc files via extern
//
mongoc_client_pool_t*  poolP   = NULL;



// -----------------------------------------------------------------------------
//
// mongocInit -
//
int mongocInit(void)
{
  //
  // The global DB (mongocGlobalDb, --globalDb, default "coraine") is reserved
  // for JSON-LD context persistence (NGSI-LD § 5.13 Context Hosting). Rejecting
  // it as the tenant DB name prevents silent collisions with the context store.
  //
  if (mongocDbName != NULL && mongocGlobalDb != NULL && strcmp(mongocDbName, mongocGlobalDb) == 0)
  {
    COR_E("mongoc: '%s' is the reserved global database name (used for JSON-LD context persistence); pick another -dbName or change --globalDb", mongocDbName);
    return -1;
  }

  mongoc_init();
  geoMatchInit();

  //
  // Build URI string
  //
  char uriStr[512];

  if (mongocDbURI != NULL)
  {
    snprintf(uriStr, sizeof(uriStr), "%s", mongocDbURI);
  }
  else if (mongocDbUser != NULL && mongocDbPwd != NULL)
  {
    snprintf(uriStr, sizeof(uriStr), "mongodb://%s:%s@%s:%u", mongocDbUser, mongocDbPwd, mongocDbHost, mongocDbPort);
  }
  else
  {
    snprintf(uriStr, sizeof(uriStr), "mongodb://%s:%u", mongocDbHost, mongocDbPort);
  }

  snprintf(mongocUriString, sizeof(mongocUriString), "%s", uriStr);

  //
  // Parse URI
  //
  bson_error_t  error;
  mongoc_uri_t* uriP = mongoc_uri_new_with_error(uriStr, &error);

  if (uriP == NULL)
  {
    COR_E("mongoc: invalid URI '%s': %s", uriStr, error.message);
    mongoc_cleanup();
    return -1;
  }

  //
  // Set server selection timeout
  //
  mongoc_uri_set_option_as_int32(uriP, MONGOC_URI_SERVERSELECTIONTIMEOUTMS, mongocDbTimeout * 1000);

  //
  // Create client pool
  //
  poolP = mongoc_client_pool_new(uriP);
  mongoc_uri_destroy(uriP);

  if (poolP == NULL)
  {
    COR_E("mongoc: failed to create client pool");
    mongoc_cleanup();
    return -1;
  }

  //
  // Verify connection with a ping
  //
  COR_I("mongoc: attempting to connect to %s, will timeout after %d seconds", uriStr, mongocDbTimeout);

  mongoc_client_t* clientP = mongoc_client_pool_pop(poolP);
  bson_t           ping;
  bson_t           reply;
  bool             ok;

  bson_init(&ping);
  BSON_APPEND_INT32(&ping, "ping", 1);

  ok = mongoc_client_command_simple(clientP, "admin", &ping, NULL, &reply, &error);
  bson_destroy(&ping);
  bson_destroy(&reply);

  if (!ok)
  {
    mongoc_client_pool_push(poolP, clientP);
    COR_E("mongoc: ping failed: %s", error.message);
    mongoc_client_pool_destroy(poolP);
    poolP = NULL;
    mongoc_cleanup();
    return -1;
  }

  COR_I("mongoc: connected to %s, database '%s'", uriStr, mongocDbName);
  mongocServerVersionGet();

  //
  // The existing tenant databases: any DB named "{prefix}-*"
  //
  char**  dbNames   = mongoc_client_get_database_names_with_opts(clientP, NULL, &error);
  int     prefixLen = strlen(mongocDbName);

  mongoc_client_pool_push(poolP, clientP);

  //
  // The storage format of every database of the broker - the default tenant's and each tenant's -
  // before anything is written to any of them: one in a newer format than this build knows, and
  // the broker does not start, with all of them as they were (mongocStorageFormat says why)
  //
  bool refused = (mongocStorageFormat(mongocDbName, false) != 0);

  for (int ix = 0; (dbNames != NULL) && (dbNames[ix] != NULL); ix++)
  {
    if ((strncmp(dbNames[ix], mongocDbName, prefixLen) == 0) && (dbNames[ix][prefixLen] == '-') && (dbNames[ix][prefixLen + 1] != 0))
    {
      if (mongocStorageFormat(dbNames[ix], false) != 0)
        refused = true;
    }
  }

  if (refused)
  {
    bson_strfreev(dbNames);
    return -1;
  }

  //
  // Bind the default tenant to the configured database and set up its indexes.
  // tenant0 and its caches were already created by main's tenantInit() before
  // DB start; here we only point them at the real db name (re-running
  // tenantInit would orphan those caches — a startup leak).
  //
  tenantDbPrefixSet(mongocDbName);
  if (mongocTenantSetup(&tenant0) != 0)
  {
    bson_strfreev(dbNames);
    return -1;
  }

  if (dbNames != NULL)
  {
    for (int ix = 0; dbNames[ix] != NULL; ix++)
    {
      // Match databases named "{prefix}-{tenantname}"
      if (strncmp(dbNames[ix], mongocDbName, prefixLen) != 0)
        continue;
      if (dbNames[ix][prefixLen] != '-')
        continue;

      const char* tenantName = &dbNames[ix][prefixLen + 1];
      if (tenantName[0] == 0)
        continue;

      Tenant* tP = tenantGetOrCreate(tenantName);
      if (tP != NULL && !tP->initialized)
      {
        mongocTenantSetup(tP);
        tP->initialized = true;
        COR_I("mongoc: discovered tenant '%s' (db: '%s')", tP->name, tP->dbName);
      }
    }

    bson_strfreev(dbNames);
  }

  return 0;
}
