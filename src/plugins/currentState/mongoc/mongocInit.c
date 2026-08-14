//
// FILE            mongocInit.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stdio.h>                                   // snprintf
#include <string.h>                                  // strdup, strlen, strncmp, strcmp

#include <mongoc/mongoc.h>                           // mongoc_init, mongoc_client_pool_new, ...

#include "ktrace/kTrace.h"                               // KT_I, KT_E

#include "db/Tenant.h"                               // tenantInit, tenantGetOrCreate, tenant0
#include "shared/geoMatch.h"                                      // geoMatchInit
#include "currentState/mongoc/mongocGeoIndex.h"                   // mongocGeoIndexInit
#include "currentState/mongoc/mongocGlobals.h"                    // mongocDbHost, mongocDbName, ...
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
  // The global DB (mongocGlobalDb, --globalDb, default "swBroker") is reserved
  // for JSON-LD context persistence (NGSI-LD § 5.13 Context Hosting). Rejecting
  // it as the tenant DB name prevents silent collisions with the context store.
  //
  if (mongocDbName != NULL && mongocGlobalDb != NULL && strcmp(mongocDbName, mongocGlobalDb) == 0)
  {
    KT_E("mongoc: '%s' is the reserved global database name (used for JSON-LD context persistence); pick another -dbName or change --globalDb", mongocDbName);
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
    KT_E("mongoc: invalid URI '%s': %s", uriStr, error.message);
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
    KT_E("mongoc: failed to create client pool");
    mongoc_cleanup();
    return -1;
  }

  //
  // Verify connection with a ping
  //
  KT_I("mongoc: attempting to connect to %s, will timeout after %d seconds", uriStr, mongocDbTimeout);

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
    KT_E("mongoc: ping failed: %s", error.message);
    mongoc_client_pool_destroy(poolP);
    poolP = NULL;
    mongoc_cleanup();
    return -1;
  }

  KT_I("mongoc: connected to %s, database '%s'", uriStr, mongocDbName);
  mongocServerVersionGet();

  //
  // Bind the default tenant to the configured database and set up its indexes.
  // tenant0 and its caches were already created by main's tenantInit() before
  // DB start; here we only point them at the real db name (re-running
  // tenantInit would orphan those caches — a startup leak).
  //
  tenantDbPrefixSet(mongocDbName);
  mongocTenantSetup(&tenant0);

  //
  // Discover existing tenant databases: any DB named "{prefix}-*"
  //
  char**  dbNames = mongoc_client_get_database_names_with_opts(clientP, NULL, &error);

  mongoc_client_pool_push(poolP, clientP);

  if (dbNames != NULL)
  {
    int prefixLen = strlen(mongocDbName);

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
        KT_I("mongoc: discovered tenant '%s' (db: '%s')", tP->name, tP->dbName);
      }
    }

    bson_strfreev(dbNames);
  }

  return 0;
}
