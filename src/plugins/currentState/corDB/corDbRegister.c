//
// FILE            corDbRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#define PLUGIN_VERSION "0.2.0"

#include <string.h>                                    // memset

#include "corNgsild/LdSubCache.h"                       // LdSubCacheItem
#include "db/DbDriver.h"                               // DbDriver
#include "db/DbQueryFilter.h"                          // DbQueryFilter
#include "shared/geoMatch.h"                           // csrGeoMatchOverlap

#include "currentState/corDB/corDbGlobals.h"        // corDbArgV
#include "currentState/corDB/corDbInit.h"           // corDbInit
#include "currentState/corDB/corDbClose.h"          // corDbClose
#include "currentState/corDB/corDbStore.h"          // corDbTenantStore
#include "currentState/corDB/corDbEntityCreate.h"   // corDbEntityCreate
#include "currentState/corDB/corDbEntityBulkCreate.h" // corDbEntityBulkCreate
#include "currentState/corDB/corDbEntityBulkUpdate.h" // corDbEntityBulkUpdate
#include "currentState/corDB/corDbEntityBulkMerge.h"  // corDbEntityBulkMerge
#include "currentState/corDB/corDbEntityBulkDelete.h" // corDbEntityBulkDelete
#include "currentState/corDB/corDbEntityRetrieve.h" // corDbEntityRetrieve
#include "currentState/corDB/corDbEntityQuery.h"    // corDbEntityQuery
#include "currentState/corDB/corDbEntityDelete.h"   // corDbEntityDelete
#include "currentState/corDB/corDbEntityMerge.h"    // corDbEntityMerge
#include "currentState/corDB/corDbEntityReplace.h"  // corDbEntityReplace
#include "currentState/corDB/corDbEntityAttrsSet.h" // corDbEntityAttrsSet
#include "currentState/corDB/corDbTypeList.h"       // corDbTypeList
#include "currentState/corDB/corDbAttrList.h"       // corDbAttrList
#if COR_FEATURE_SUBSCRIPTIONS
#include "currentState/corDB/corDbSubscriptionCreate.h"    // corDbSubscriptionCreate
#include "currentState/corDB/corDbSubscriptionRetrieve.h"  // corDbSubscriptionRetrieve
#include "currentState/corDB/corDbSubscriptionQuery.h"     // corDbSubscriptionQuery
#include "currentState/corDB/corDbSubscriptionUpdate.h"    // corDbSubscriptionUpdate
#include "currentState/corDB/corDbSubscriptionReplace.h"   // corDbSubscriptionReplace
#include "currentState/corDB/corDbSubscriptionDelete.h"    // corDbSubscriptionDelete
#endif
#if COR_FEATURE_REGISTRATIONS
#include "currentState/corDB/corDbRegistrationCreate.h"    // corDbRegistrationCreate
#include "currentState/corDB/corDbRegistrationRetrieve.h"  // corDbRegistrationRetrieve
#include "currentState/corDB/corDbRegistrationQuery.h"     // corDbRegistrationQuery
#include "currentState/corDB/corDbRegistrationUpdate.h"    // corDbRegistrationUpdate
#include "currentState/corDB/corDbRegistrationDelete.h"    // corDbRegistrationDelete
#endif
#include "currentState/corDB/corDbGeoMatch.h"             // corDbGeoMatch



// -----------------------------------------------------------------------------
//
// corDbGeoMatchCb - generic geo match callback
//
static bool corDbGeoMatchCb(KjNode* entityP, LdGeoRel* geoRel, const char* geometry,
                             const char* coordinates, const char* geoproperty)
{
  DbQueryFilter filter;

  memset(&filter, 0, sizeof(filter));
  filter.geoRel       = geoRel;
  filter.geometry      = (char*) geometry;
  filter.coordinates   = (char*) coordinates;
  filter.geoproperty   = (char*) geoproperty;

  return corDbGeoMatch(entityP, &filter, NULL);
}



// -----------------------------------------------------------------------------
//
// corDbCsrGeoMatchCb - geoQ ↔ CSR geo-coverage match (§ 5.10.2.4)
//
static bool corDbCsrGeoMatchCb(KjNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates)
{
  return csrGeoMatchOverlap(csrGeoP, geoRel, geometry, coordinates);
}



// -----------------------------------------------------------------------------
//
// corDbTenantSetup - create the per-tenant store tree on first use
//
static int corDbTenantSetup(Tenant* tenantP)
{
  corDbTenantStore(tenantP);
  return 0;
}



// -----------------------------------------------------------------------------
//
// dbRegister -
//
void dbRegister(DbDriver* driverP)
{
  driverP->alias           = "corDB";
  driverP->version         = PLUGIN_VERSION;
  driverP->args            = corDbArgV;
  driverP->init            = corDbInit;
  driverP->close           = corDbClose;
  driverP->entityCreate     = corDbEntityCreate;
  driverP->entityBulkCreate = corDbEntityBulkCreate;
  driverP->entityBulkUpdate = corDbEntityBulkUpdate;
  driverP->entityBulkRetrieve     = corDbEntityBulkRetrieve;
  driverP->entityBulkChangesApply = corDbEntityBulkChangesApply;
  driverP->entityBulkDelete = corDbEntityBulkDelete;
  driverP->entityRetrieve  = corDbEntityRetrieve;
  driverP->entityQuery     = corDbEntityQuery;
  driverP->entityDelete    = corDbEntityDelete;
  driverP->entityChangesApply = corDbEntityChangesApply;
  driverP->entityReplace   = corDbEntityReplace;
  driverP->entityAttrsSet  = corDbEntityAttrsSet;
  driverP->typeList        = corDbTypeList;
  driverP->attrList        = corDbAttrList;
  //
  // With COR_FEATURE_SUBSCRIPTIONS off these sources leave the plugin build
  // (CMakeLists.txt) and the slots stay NULL - the driver-slot convention for
  // "this driver does not do that". Nothing calls them: the routes that would
  // are answered by corNotInThisBuild before any driver is reached.
  //
#if COR_FEATURE_SUBSCRIPTIONS
  driverP->subscriptionCreate   = corDbSubscriptionCreate;
  driverP->subscriptionRetrieve = corDbSubscriptionRetrieve;
  driverP->subscriptionQuery    = corDbSubscriptionQuery;
  driverP->subscriptionUpdate   = corDbSubscriptionUpdate;
  driverP->subscriptionReplace  = corDbSubscriptionReplace;
  driverP->subscriptionDelete   = corDbSubscriptionDelete;
  driverP->subscriptionList     = corDbSubscriptions;
#endif
  //
  // With COR_FEATURE_REGISTRATIONS off these sources leave the plugin build
  // (CMakeLists.txt) and the slots stay NULL - the driver-slot convention for
  // "this driver does not do that". Nothing calls them: the routes that would
  // are answered by corNotInThisBuild before any driver is reached.
  //
#if COR_FEATURE_REGISTRATIONS
  driverP->registrationCreate   = corDbRegistrationCreate;
  driverP->registrationRetrieve = corDbRegistrationRetrieve;
  driverP->registrationQuery    = corDbRegistrationQuery;
  driverP->registrationUpdate   = corDbRegistrationUpdate;
  driverP->registrationDelete   = corDbRegistrationDelete;
  driverP->registrationList     = corDbRegistrations;
#endif
  driverP->tenantSetup     = corDbTenantSetup;
  driverP->geoMatchFunc    = corDbGeoMatchCb;
  driverP->csrGeoMatchFunc = corDbCsrGeoMatchCb;
}
