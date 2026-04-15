//
// FILE            ramdbRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#define PLUGIN_VERSION "0.2.0"

#include <string.h>                                    // memset

#include "swNgsild/LdSubCache.h"                       // LdSubCacheItem
#include "db/DbDriver.h"                               // DbDriver
#include "db/DbQueryFilter.h"                          // DbQueryFilter

#include "currentState/swRamDB/ramdbGlobals.h"        // ramdbArgV
#include "currentState/swRamDB/ramdbInit.h"           // ramdbInit
#include "currentState/swRamDB/ramdbClose.h"          // ramdbClose
#include "currentState/swRamDB/ramdbStore.h"          // ramdbTenantStore
#include "currentState/swRamDB/ramdbEntityCreate.h"   // ramdbEntityCreate
#include "currentState/swRamDB/ramdbEntityRetrieve.h" // ramdbEntityRetrieve
#include "currentState/swRamDB/ramdbEntityQuery.h"    // ramdbEntityQuery
#include "currentState/swRamDB/ramdbEntityDelete.h"   // ramdbEntityDelete
#include "currentState/swRamDB/ramdbEntityMerge.h"    // ramdbEntityMerge
#include "currentState/swRamDB/ramdbEntityReplace.h"  // ramdbEntityReplace
#include "currentState/swRamDB/ramdbSubscriptionCreate.h"    // ramdbSubscriptionCreate
#include "currentState/swRamDB/ramdbSubscriptionRetrieve.h"  // ramdbSubscriptionRetrieve
#include "currentState/swRamDB/ramdbSubscriptionQuery.h"     // ramdbSubscriptionQuery
#include "currentState/swRamDB/ramdbSubscriptionUpdate.h"    // ramdbSubscriptionUpdate
#include "currentState/swRamDB/ramdbSubscriptionDelete.h"    // ramdbSubscriptionDelete
#include "currentState/swRamDB/ramdbGeoMatch.h"             // ramdbGeoMatch



// -----------------------------------------------------------------------------
//
// ramdbSubGeoMatch - callback adapter: LdSubCacheItem → DbQueryFilter for ramdbGeoMatch
//
static bool ramdbSubGeoMatch(KjNode* entityP, LdSubCacheItem* itemP)
{
  DbQueryFilter filter;

  memset(&filter, 0, sizeof(filter));
  filter.geoRel       = itemP->geoRel;
  filter.geometry      = itemP->geoGeometry;
  filter.coordinates   = itemP->geoCoordinates;
  filter.geoproperty   = itemP->geoProperty;

  return ramdbGeoMatch(entityP, &filter, NULL);
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
  driverP->alias           = "swRamDB";
  driverP->version         = PLUGIN_VERSION;
  driverP->args            = ramdbArgV;
  driverP->init            = ramdbInit;
  driverP->close           = ramdbClose;
  driverP->entityCreate    = ramdbEntityCreate;
  driverP->entityRetrieve  = ramdbEntityRetrieve;
  driverP->entityQuery     = ramdbEntityQuery;
  driverP->entityDelete    = ramdbEntityDelete;
  driverP->entityMerge     = ramdbEntityMerge;
  driverP->entityReplace   = ramdbEntityReplace;
  driverP->subscriptionCreate   = ramdbSubscriptionCreate;
  driverP->subscriptionRetrieve = ramdbSubscriptionRetrieve;
  driverP->subscriptionQuery    = ramdbSubscriptionQuery;
  driverP->subscriptionUpdate   = ramdbSubscriptionUpdate;
  driverP->subscriptionDelete   = ramdbSubscriptionDelete;
  driverP->subscriptionList     = ramdbSubscriptions;
  driverP->tenantSetup     = ramdbTenantSetup;
  driverP->geoMatchFunc    = ramdbSubGeoMatch;
}
