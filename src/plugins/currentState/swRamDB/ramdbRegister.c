//
// FILE            ramdbRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#define PLUGIN_VERSION "0.2.0"

#include "db/DbDriver.h"                              // DbDriver

#include "currentState/swRamDB/ramdbGlobals.h"        // ramdbArgV
#include "currentState/swRamDB/ramdbInit.h"           // ramdbInit
#include "currentState/swRamDB/ramdbClose.h"          // ramdbClose
#include "currentState/swRamDB/ramdbStore.h"          // ramdbTenantStore
#include "currentState/swRamDB/ramdbEntityCreate.h"   // ramdbEntityCreate
#include "currentState/swRamDB/ramdbEntityRetrieve.h" // ramdbEntityRetrieve
#include "currentState/swRamDB/ramdbEntityQuery.h"    // ramdbEntityQuery
#include "currentState/swRamDB/ramdbEntityDelete.h"   // ramdbEntityDelete



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
  driverP->tenantSetup     = ramdbTenantSetup;
}
