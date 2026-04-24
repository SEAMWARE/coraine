//
// FILE            adminSubStats.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                   // NULL

#include "swRest/SwRestState.h"                       // swRest

#include "swNgsild/LdSubCache.h"                      // LdSubCache
#include "swNgsild/ldSubStatsFlush.h"                 // ldSubStatsFlush

#include "db/DbDriver.h"                              // db (DbDriver)
#include "db/Tenant.h"                                // tenant0, tenantList

#include "api/admin/adminSubStats.h"                  // Own interface



// -----------------------------------------------------------------------------
//
// dbFlushAdapter - bridges the DbDriver's stats-flush function-pointer
// signature (Tenant*) to the swNgsild-side generic signature (void*).
//
static int dbFlushAdapter(void*        tenantP,
                          const char*  subId,
                          int          deltaSent,
                          int          deltaFailed,
                          uint64_t     lastNotification,
                          uint64_t     lastSuccess,
                          uint64_t     lastFailure)
{
  if (db.subscriptionStatsFlush == NULL)
    return -1;
  return db.subscriptionStatsFlush((Tenant*) tenantP, subId,
                                   deltaSent, deltaFailed,
                                   lastNotification, lastSuccess, lastFailure);
}



// -----------------------------------------------------------------------------
//
// adminPostSubStatsFlush -
//
bool adminPostSubStatsFlush(void)
{
  // Walk every tenant (default + linked list) and flush both caches
  for (Tenant* tP = &tenant0;
       tP != NULL;
       tP = (tP == &tenant0) ? tenantList : tP->next)
  {
    if (tP->subCacheP != NULL)
      ldSubStatsFlush(tP, (LdSubCache*) tP->subCacheP, dbFlushAdapter);

    if (tP->regSubCacheP != NULL)
      ldSubStatsFlush(tP, (LdSubCache*) tP->regSubCacheP, dbFlushAdapter);
  }

  swRest.out.httpStatusCode = 204;
  return true;
}
