#ifndef DB_DBDRIVER_H_
#define DB_DBDRIVER_H_

//
// FILE            DbDriver.h
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//

#include <stdint.h>                                       // uint64_t

#include "kalloc/KAlloc.h"                                // KAlloc
#include "kargs/KArg.h"                                   // KArg
#include "kjson/KjNode.h"                                 // KjNode

#include "swNgsild/ldEntityMerge.h"                       // LdMergeReport
#include "swNgsild/LdSubCache.h"                          // LdSubGeoMatchFunc

#include "db/DbQueryFilter.h"                             // DbQueryFilter
#include "db/Tenant.h"                                    // Tenant



// -----------------------------------------------------------------------------
//
// Error codes
//
#define DB_OK               0
#define DB_ERR              -1
#define DB_ALREADY_EXISTS   -2
#define DB_NOT_FOUND        -3



// -----------------------------------------------------------------------------
//
// Function pointer types
//
typedef int  (*DbInitFunc)(void);
typedef void (*DbCloseFunc)(void);
typedef int  (*DbEntityCreateFunc)(Tenant* tenantP, const char* entityId, KjNode* entityP);
typedef int  (*DbEntityRetrieveFunc)(Tenant* tenantP, const char* entityId, KjNode** entityPP);
typedef int  (*DbEntityQueryFunc)(Tenant* tenantP, DbQueryFilter* filterP, KjNode** arrayPP);
typedef int  (*DbEntityDeleteFunc)(Tenant* tenantP, const char* entityId);
typedef int  (*DbEntityMergeFunc)(Tenant* tenantP, const char* entityId, KjNode* fragmentDb,
                                  uint64_t ts, LdMergeReport* reportP);
typedef int  (*DbEntityReplaceFunc)(Tenant* tenantP, const char* entityId,
                                    KjNode* newEntityP, KjNode** oldEntityPP);
typedef int  (*DbSubscriptionCreateFunc)(Tenant* tenantP, const char* subId, KjNode* subP);
typedef int  (*DbSubscriptionRetrieveFunc)(Tenant* tenantP, const char* subId, KjNode** subPP);
typedef int  (*DbSubscriptionQueryFunc)(Tenant* tenantP, int limit, int offset, KjNode** arrayPP);
typedef int  (*DbSubscriptionUpdateFunc)(Tenant* tenantP, const char* subId, KjNode* fragmentP);
typedef int  (*DbSubscriptionDeleteFunc)(Tenant* tenantP, const char* subId);
typedef KjNode* (*DbSubscriptionListFunc)(Tenant* tenantP);
typedef int  (*DbTenantSetupFunc)(Tenant* tenantP);
typedef void (*DbVersionInfoFunc)(KAlloc* allocP, KjNode* root);



// -----------------------------------------------------------------------------
//
// DbDriver -
//
typedef struct DbDriver
{
  const char*             alias;           // short plugin name, e.g. "mongoc" (for usage text)
  const char*             version;         // plugin version string
  KArg*                   args;            // plugin-contributed CLI args (NULL if none)
  DbInitFunc              init;
  DbCloseFunc             close;
  DbEntityCreateFunc      entityCreate;
  DbEntityRetrieveFunc    entityRetrieve;
  DbEntityQueryFunc       entityQuery;
  DbEntityDeleteFunc      entityDelete;
  DbEntityMergeFunc       entityMerge;
  DbEntityReplaceFunc     entityReplace;
  DbSubscriptionCreateFunc   subscriptionCreate;
  DbSubscriptionRetrieveFunc subscriptionRetrieve;
  DbSubscriptionQueryFunc    subscriptionQuery;
  DbSubscriptionUpdateFunc   subscriptionUpdate;
  DbSubscriptionDeleteFunc   subscriptionDelete;
  DbSubscriptionListFunc     subscriptionList;
  DbTenantSetupFunc       tenantSetup;
  DbVersionInfoFunc       versionInfo;
  LdSubGeoMatchFunc       geoMatchFunc;    // geo match callback for subscription notifications
} DbDriver;



// -----------------------------------------------------------------------------
//
// DbRegisterFunc -
//
typedef void (*DbRegisterFunc)(DbDriver* driverP);



// -----------------------------------------------------------------------------
//
// dbDriver - global driver instance
//
extern DbDriver db;

#endif  // DB_DBDRIVER_H_
