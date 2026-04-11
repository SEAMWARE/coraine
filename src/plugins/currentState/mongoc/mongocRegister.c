//
// FILE            mongocRegister.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include "db/DbDriver.h"                              // DbDriver

#include "currentState/mongoc/mongocGlobals.h"                      // mongocArgV
#include "currentState/mongoc/mongocInit.h"                         // mongocInit
#include "currentState/mongoc/mongocClose.h"                        // mongocClose
#include "currentState/mongoc/mongocEntityCreate.h"                 // mongocEntityCreate
#include "currentState/mongoc/mongocEntityRetrieve.h"               // mongocEntityRetrieve
#include "currentState/mongoc/mongocEntityQuery.h"                  // mongocEntityQuery
#include "currentState/mongoc/mongocEntityDelete.h"                 // mongocEntityDelete
#include "currentState/mongoc/mongocEntityMerge.h"                  // mongocEntityMerge
#include "currentState/mongoc/mongocTenantSetup.h"                  // mongocTenantSetup
#include "currentState/mongoc/mongocVersion.h"                     // mongocVersionInfo



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
  driverP->tenantSetup     = mongocTenantSetup;
  driverP->versionInfo     = mongocVersionInfo;
}
