//
// FILE            ramdbRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#define PLUGIN_VERSION "0.2.0"

#include <string.h>                                    // memset

#include "corNgsild/LdSubCache.h"                       // LdSubCacheItem
#include "db/DbDriver.h"                               // DbDriver
#include "db/DbQueryFilter.h"                          // DbQueryFilter
#include "shared/geoMatch.h"                           // csrGeoMatchOverlap

#include "currentState/corRamDB/ramdbGlobals.h"        // ramdbArgV
#include "currentState/corRamDB/ramdbInit.h"           // ramdbInit
#include "currentState/corRamDB/ramdbClose.h"          // ramdbClose
#include "currentState/corRamDB/ramdbStore.h"          // ramdbTenantStore
#include "currentState/corRamDB/ramdbEntityCreate.h"   // ramdbEntityCreate
#include "currentState/corRamDB/ramdbEntityBulkCreate.h" // ramdbEntityBulkCreate
#include "currentState/corRamDB/ramdbEntityBulkUpdate.h" // ramdbEntityBulkUpdate
#include "currentState/corRamDB/ramdbEntityBulkMerge.h"  // ramdbEntityBulkMerge
#include "currentState/corRamDB/ramdbEntityBulkDelete.h" // ramdbEntityBulkDelete
#include "currentState/corRamDB/ramdbEntityRetrieve.h" // ramdbEntityRetrieve
#include "currentState/corRamDB/ramdbEntityQuery.h"    // ramdbEntityQuery
#include "currentState/corRamDB/ramdbEntityDelete.h"   // ramdbEntityDelete
#include "currentState/corRamDB/ramdbEntityMerge.h"    // ramdbEntityMerge
#include "currentState/corRamDB/ramdbEntityReplace.h"  // ramdbEntityReplace
#include "currentState/corRamDB/ramdbEntityAttrsSet.h" // ramdbEntityAttrsSet
#include "currentState/corRamDB/ramdbTypeList.h"       // ramdbTypeList
#include "currentState/corRamDB/ramdbAttrList.h"       // ramdbAttrList
#include "currentState/corRamDB/ramdbSubscriptionCreate.h"    // ramdbSubscriptionCreate
#include "currentState/corRamDB/ramdbSubscriptionRetrieve.h"  // ramdbSubscriptionRetrieve
#include "currentState/corRamDB/ramdbSubscriptionQuery.h"     // ramdbSubscriptionQuery
#include "currentState/corRamDB/ramdbSubscriptionUpdate.h"    // ramdbSubscriptionUpdate
#include "currentState/corRamDB/ramdbSubscriptionReplace.h"   // ramdbSubscriptionReplace
#include "currentState/corRamDB/ramdbSubscriptionDelete.h"    // ramdbSubscriptionDelete
#include "currentState/corRamDB/ramdbRegistrationCreate.h"    // ramdbRegistrationCreate
#include "currentState/corRamDB/ramdbRegistrationRetrieve.h"  // ramdbRegistrationRetrieve
#include "currentState/corRamDB/ramdbRegistrationQuery.h"     // ramdbRegistrationQuery
#include "currentState/corRamDB/ramdbRegistrationUpdate.h"    // ramdbRegistrationUpdate
#include "currentState/corRamDB/ramdbRegistrationDelete.h"    // ramdbRegistrationDelete
#include "currentState/corRamDB/ramdbGeoMatch.h"             // ramdbGeoMatch



// -----------------------------------------------------------------------------
//
// ramdbGeoMatchCb - generic geo match callback
//
static bool ramdbGeoMatchCb(KjNode* entityP, LdGeoRel* geoRel, const char* geometry,
                             const char* coordinates, const char* geoproperty)
{
  DbQueryFilter filter;

  memset(&filter, 0, sizeof(filter));
  filter.geoRel       = geoRel;
  filter.geometry      = (char*) geometry;
  filter.coordinates   = (char*) coordinates;
  filter.geoproperty   = (char*) geoproperty;

  return ramdbGeoMatch(entityP, &filter, NULL);
}



// -----------------------------------------------------------------------------
//
// ramdbCsrGeoMatchCb - geoQ ↔ CSR geo-coverage match (§ 5.10.2.4)
//
static bool ramdbCsrGeoMatchCb(KjNode* csrGeoP, LdGeoRel* geoRel, const char* geometry, const char* coordinates)
{
  return csrGeoMatchOverlap(csrGeoP, geoRel, geometry, coordinates);
}



// -----------------------------------------------------------------------------
//
// ramdbTenantSetup - create the per-tenant store tree on first use
//
static int ramdbTenantSetup(Tenant* tenantP)
{
  ramdbTenantStore(tenantP);
  return 0;
}



// -----------------------------------------------------------------------------
//
// dbRegister -
//
void dbRegister(DbDriver* driverP)
{
  driverP->alias           = "corRamDB";
  driverP->version         = PLUGIN_VERSION;
  driverP->args            = ramdbArgV;
  driverP->init            = ramdbInit;
  driverP->close           = ramdbClose;
  driverP->entityCreate     = ramdbEntityCreate;
  driverP->entityBulkCreate = ramdbEntityBulkCreate;
  driverP->entityBulkUpdate = ramdbEntityBulkUpdate;
  driverP->entityBulkRetrieve     = ramdbEntityBulkRetrieve;
  driverP->entityBulkChangesApply = ramdbEntityBulkChangesApply;
  driverP->entityBulkDelete = ramdbEntityBulkDelete;
  driverP->entityRetrieve  = ramdbEntityRetrieve;
  driverP->entityQuery     = ramdbEntityQuery;
  driverP->entityDelete    = ramdbEntityDelete;
  driverP->entityChangesApply = ramdbEntityChangesApply;
  driverP->entityReplace   = ramdbEntityReplace;
  driverP->entityAttrsSet  = ramdbEntityAttrsSet;
  driverP->typeList        = ramdbTypeList;
  driverP->attrList        = ramdbAttrList;
  driverP->subscriptionCreate   = ramdbSubscriptionCreate;
  driverP->subscriptionRetrieve = ramdbSubscriptionRetrieve;
  driverP->subscriptionQuery    = ramdbSubscriptionQuery;
  driverP->subscriptionUpdate   = ramdbSubscriptionUpdate;
  driverP->subscriptionReplace  = ramdbSubscriptionReplace;
  driverP->subscriptionDelete   = ramdbSubscriptionDelete;
  driverP->subscriptionList     = ramdbSubscriptions;
  driverP->registrationCreate   = ramdbRegistrationCreate;
  driverP->registrationRetrieve = ramdbRegistrationRetrieve;
  driverP->registrationQuery    = ramdbRegistrationQuery;
  driverP->registrationUpdate   = ramdbRegistrationUpdate;
  driverP->registrationDelete   = ramdbRegistrationDelete;
  driverP->registrationList     = ramdbRegistrations;
  driverP->tenantSetup     = ramdbTenantSetup;
  driverP->geoMatchFunc    = ramdbGeoMatchCb;
  driverP->csrGeoMatchFunc = ramdbCsrGeoMatchCb;
}
