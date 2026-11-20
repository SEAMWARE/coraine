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
#include <strings.h>                             // strcasecmp

#include <mongoc/mongoc.h>                           // mongoc_init, mongoc_client_pool_new, ...

#include "corLog/corLog.h"                               // COR_I, COR_E
#include "corArgs/corArgsSecret.h"                // corArgsSecret
#include "corBase/corUriPasswordMask.h"                   // corUriPasswordMask

#include "db/Tenant.h"                               // tenantInit, tenantGetOrCreate, tenant0
#include "shared/geoMatch.h"                                      // geoMatchInit
#include "currentState/mongoc/mongocGeoIndex.h"                   // mongocGeoIndexInit
#include "currentState/mongoc/mongocGlobals.h"                    // mongocDbHost, mongocDbName, ...
#include "currentState/mongoc/mongocStorageFormat.h"              // mongocStorageFormat
#include "currentState/mongoc/mongocTenantSetup.h"                // mongocTenantSetup
#include "currentState/mongoc/mongocVersion.h"                    // mongocServerVersionGet
#if COR_FEATURE_DB_INDEXES && COR_FEATURE_Q_PREFILTER
#include "currentState/mongoc/mongocIndexes.h"                    // mongocIndexesInit
#endif
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
  corArgsSecret("--dbPwd");                        // never shown: GET /admin/config masks them
  corArgsSecret("--dbURI");                        // a URI may carry user:password

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
  // The URI as the log shows it: its password "***", as GET /admin/config shows --dbPwd and --dbURI
  //
  char uriShown[512];
  corUriPasswordMask(uriStr, uriShown, sizeof(uriShown));

  //
  // Parse URI
  //
  bson_error_t  error;
  mongoc_uri_t* uriP = mongoc_uri_new_with_error(uriStr, &error);

  if (uriP == NULL)
  {
    COR_E("mongoc: invalid URI '%s': %s", uriShown, error.message);
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
  COR_I("mongoc: attempting to connect to %s, will timeout after %d seconds", uriShown, mongocDbTimeout);

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

  COR_I("mongoc: connected to %s, database '%s'", uriShown, mongocDbName);
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

#if COR_FEATURE_DB_INDEXES && COR_FEATURE_Q_PREFILTER
  //
  // The declared indexes - before any tenant is set up: each setup creates the ones that cover it
  //
  if (mongocIndexesInit() != 0)
  {
    bson_strfreev(dbNames);
    return -1;
  }
#endif

  if (mongocTenantSetup(&tenant0) != 0)
  {
    bson_strfreev(dbNames);
    return -1;
  }

  if (dbNames != NULL)
  {
    for (int ix = 0; dbNames[ix] != NULL; ix++)
    {
      // Match databases named "{prefix}-{tenantname}" - and a reserved tenant's, "{prefix}_r-{tenantname}"
      if (strncmp(dbNames[ix], mongocDbName, prefixLen) != 0)
        continue;

      const char* tenantName = NULL;
      int         markLen    = strlen(TENANT_RESERVED_DB_MARK);

      if (dbNames[ix][prefixLen] == '-')
      {
        tenantName = &dbNames[ix][prefixLen + 1];
        //
        // An ordinary tenant's database, and the name reserved (--reservedTenants): made a reserved tenant, its data
        // would never be seen. Ordinary data wins - the name stays an ordinary tenant's; the broker starts
        //
        if (tenantIsReserved(tenantName) == true)
        {
          tenantUnreserve(tenantName);
          COR_W("mongoc: '%s' is not reserved - the database '%s' holds an ordinary tenant's data under the name; move it, or reserve another name",
                tenantName, dbNames[ix]);
        }
      }
      else if (strncmp(&dbNames[ix][prefixLen], TENANT_RESERVED_DB_MARK, markLen) == 0)
      {
        tenantName = &dbNames[ix][prefixLen + markLen];

        //
        // An ordinary tenant's database of the name too: that one is served (ordinary data wins), this one left alone
        //
        bool ordinaryToo = false;
        for (int ox = 0; (dbNames[ox] != NULL) && (ordinaryToo == false); ox++)
          ordinaryToo = (strncmp(dbNames[ox], mongocDbName, prefixLen) == 0) && (dbNames[ox][prefixLen] == '-') &&
                        (strcasecmp(&dbNames[ox][prefixLen + 1], tenantName) == 0);

        if (ordinaryToo == true)
        {
          COR_E("mongoc: the database '%s' left alone - '%s' is an ordinary tenant's too (its database is served); nothing deleted", dbNames[ix], tenantName);
          continue;
        }

        if ((tenantName[0] != 0) && (tenantReserve(tenantName) == false))
          continue;                                 // the log says why
      }

      if ((tenantName == NULL) || (tenantName[0] == 0))
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
