//
// FILE            mongocRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <string.h>                                    // memset

#include "swNgsild/LdSubCache.h"                       // LdSubCacheItem
#include "db/DbDriver.h"                               // DbDriver
#include "db/DbQueryFilter.h"                          // DbQueryFilter
#include "shared/geoMatch.h"                           // geoMatch, geoMatchInit

#include "currentState/mongoc/mongocGlobals.h"                      // mongocArgV
#include "currentState/mongoc/mongocInit.h"                         // mongocInit
#include "currentState/mongoc/mongocClose.h"                        // mongocClose
#include "currentState/mongoc/mongocEntityCreate.h"                 // mongocEntityCreate
#include "currentState/mongoc/mongocEntityRetrieve.h"               // mongocEntityRetrieve
#include "currentState/mongoc/mongocEntityQuery.h"                  // mongocEntityQuery
#include "currentState/mongoc/mongocEntityDelete.h"                 // mongocEntityDelete
#include "currentState/mongoc/mongocEntityMerge.h"                  // mongocEntityMerge
#include "currentState/mongoc/mongocEntityReplace.h"                // mongocEntityReplace
#include "currentState/mongoc/mongocContext.h"                      // mongocContext*
#include "currentState/mongoc/mongocTenantSetup.h"                  // mongocTenantSetup
#include "currentState/mongoc/mongocVersion.h"                     // mongocVersionInfo
#include "currentState/mongoc/mongocSubscriptionCreate.h"           // mongocSubscriptionCreate
#include "currentState/mongoc/mongocSubscriptionRetrieve.h"         // mongocSubscriptionRetrieve
#include "currentState/mongoc/mongocSubscriptionQuery.h"            // mongocSubscriptionQuery
#include "currentState/mongoc/mongocSubscriptionUpdate.h"           // mongocSubscriptionUpdate
#include "currentState/mongoc/mongocSubscriptionDelete.h"           // mongocSubscriptionDelete



// -----------------------------------------------------------------------------
//
// mongocSubGeoMatch - callback adapter for subscription geoQ matching
//
static bool mongocSubGeoMatch(KjNode* entityP, LdSubCacheItem* itemP)
{
  DbQueryFilter filter;

  memset(&filter, 0, sizeof(filter));
  filter.geoRel       = itemP->geoRel;
  filter.geometry      = itemP->geoGeometry;
  filter.coordinates   = itemP->geoCoordinates;
  filter.geoproperty   = itemP->geoProperty;

  return geoMatch(entityP, &filter, NULL);
}



// -----------------------------------------------------------------------------
//
// dbRegister -
//
void dbRegister(DbDriver* driverP)
{
  driverP->alias           = "mongoc";
  driverP->version         = MONGOC_PLUGIN_VERSION;
  driverP->args            = mongocArgV;
  driverP->init            = mongocInit;
  driverP->close           = mongocClose;
  driverP->entityCreate    = mongocEntityCreate;
  driverP->entityRetrieve  = mongocEntityRetrieve;
  driverP->entityQuery     = mongocEntityQuery;
  driverP->entityDelete    = mongocEntityDelete;
  driverP->entityMerge     = mongocEntityMerge;
  driverP->entityReplace   = mongocEntityReplace;
  driverP->tenantSetup     = mongocTenantSetup;
  driverP->versionInfo     = mongocVersionInfo;

  driverP->subscriptionCreate    = mongocSubscriptionCreate;
  driverP->subscriptionRetrieve  = mongocSubscriptionRetrieve;
  driverP->subscriptionQuery     = mongocSubscriptionQuery;
  driverP->subscriptionUpdate    = mongocSubscriptionUpdate;
  driverP->subscriptionDelete    = mongocSubscriptionDelete;
  driverP->geoMatchFunc          = mongocSubGeoMatch;

  driverP->contextSave           = mongocContextSave;
  driverP->contextDelete         = mongocContextDelete;
  driverP->contextList           = mongocContextList;
  driverP->contextGet            = mongocContextGet;
}
