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
#include "currentState/mongoc/mongocEntityBulkCreate.h"             // mongocEntityBulkCreate
#include "currentState/mongoc/mongocEntityBulkUpdate.h"             // mongocEntityBulkUpdate
#include "currentState/mongoc/mongocEntityBulkMerge.h"              // mongocEntityBulkMerge
#include "currentState/mongoc/mongocEntityBulkDelete.h"             // mongocEntityBulkDelete
#include "currentState/mongoc/mongocEntityRetrieve.h"               // mongocEntityRetrieve
#include "currentState/mongoc/mongocEntityQuery.h"                  // mongocEntityQuery
#include "currentState/mongoc/mongocEntityDelete.h"                 // mongocEntityDelete
#include "currentState/mongoc/mongocEntityMerge.h"                  // mongocEntityMerge
#include "currentState/mongoc/mongocEntityReplace.h"                // mongocEntityReplace
#include "currentState/mongoc/mongocEntityAttrsSet.h"               // mongocEntityAttrsSet
#include "currentState/mongoc/mongocTypeList.h"                     // mongocTypeList
#include "currentState/mongoc/mongocAttrList.h"                     // mongocAttrList
#include "currentState/mongoc/mongocContext.h"                      // mongocContext*
#include "currentState/mongoc/mongocTenantSetup.h"                  // mongocTenantSetup
#include "currentState/mongoc/mongocVersion.h"                     // mongocVersionInfo
#include "currentState/mongoc/mongocSubscriptionCreate.h"           // mongocSubscriptionCreate
#include "currentState/mongoc/mongocSubscriptionRetrieve.h"         // mongocSubscriptionRetrieve
#include "currentState/mongoc/mongocSubscriptionQuery.h"            // mongocSubscriptionQuery
#include "currentState/mongoc/mongocSubscriptionUpdate.h"           // mongocSubscriptionUpdate
#include "currentState/mongoc/mongocSubscriptionDelete.h"           // mongocSubscriptionDelete
#include "currentState/mongoc/mongocSubscriptionStatsFlush.h"       // mongocSubscriptionStatsFlush
#include "currentState/mongoc/mongocRegistrationCreate.h"           // mongocRegistrationCreate
#include "currentState/mongoc/mongocRegistrationRetrieve.h"         // mongocRegistrationRetrieve
#include "currentState/mongoc/mongocRegistrationQuery.h"            // mongocRegistrationQuery
#include "currentState/mongoc/mongocRegistrationUpdate.h"           // mongocRegistrationUpdate
#include "currentState/mongoc/mongocRegistrationDelete.h"           // mongocRegistrationDelete



// -----------------------------------------------------------------------------
//
// mongocGeoMatchCb - generic geo match callback
//
static bool mongocGeoMatchCb(KjNode* entityP, LdGeoRel* geoRel, const char* geometry,
                              const char* coordinates, const char* geoproperty)
{
  DbQueryFilter filter;

  memset(&filter, 0, sizeof(filter));
  filter.geoRel       = geoRel;
  filter.geometry      = (char*) geometry;
  filter.coordinates   = (char*) coordinates;
  filter.geoproperty   = (char*) geoproperty;

  return geoMatch(entityP, &filter, NULL);
}



// -----------------------------------------------------------------------------
//
// mongocCsrGeoMatchCb - geoQ ↔ CSR geo-coverage match (§ 5.10.2.4)
//
static bool mongocCsrGeoMatchCb(KjNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates)
{
  return csrGeoMatchOverlap(csrGeoP, geoRel, geometry, coordinates);
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
  driverP->entityCreate     = mongocEntityCreate;
  driverP->entityBulkCreate = mongocEntityBulkCreate;
  driverP->entityBulkUpdate = mongocEntityBulkUpdate;
  driverP->entityBulkMerge  = mongocEntityBulkMerge;
  driverP->entityBulkDelete = mongocEntityBulkDelete;
  driverP->entityRetrieve  = mongocEntityRetrieve;
  driverP->entityQuery     = mongocEntityQuery;
  driverP->entityDelete    = mongocEntityDelete;
  driverP->entityMerge     = mongocEntityMerge;
  driverP->entityReplace   = mongocEntityReplace;
  driverP->entityAttrsSet  = mongocEntityAttrsSet;
  driverP->typeList        = mongocTypeList;
  driverP->attrList        = mongocAttrList;
  driverP->tenantSetup     = mongocTenantSetup;
  driverP->versionInfo     = mongocVersionInfo;

  driverP->subscriptionCreate    = mongocSubscriptionCreate;
  driverP->subscriptionRetrieve  = mongocSubscriptionRetrieve;
  driverP->subscriptionQuery     = mongocSubscriptionQuery;
  driverP->subscriptionUpdate    = mongocSubscriptionUpdate;
  driverP->subscriptionDelete    = mongocSubscriptionDelete;
  driverP->subscriptionStatsFlush = mongocSubscriptionStatsFlush;
  driverP->geoMatchFunc          = mongocGeoMatchCb;
  driverP->csrGeoMatchFunc       = mongocCsrGeoMatchCb;

  driverP->registrationCreate    = mongocRegistrationCreate;
  driverP->registrationRetrieve  = mongocRegistrationRetrieve;
  driverP->registrationQuery     = mongocRegistrationQuery;
  driverP->registrationUpdate    = mongocRegistrationUpdate;
  driverP->registrationDelete    = mongocRegistrationDelete;

  driverP->contextSave           = mongocContextSave;
  driverP->contextDelete         = mongocContextDelete;
  driverP->contextList           = mongocContextList;
  driverP->contextGet            = mongocContextGet;
}
