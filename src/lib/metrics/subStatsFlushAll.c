//
// FILE            subStatsFlushAll.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <stddef.h>                                   // NULL
#include <stdint.h>                                   // uint64_t

#include "swNgsild/LdSubCache.h"                      // LdSubCache
#include "swNgsild/LdPernotCache.h"                   // LdPernotCache
#include "swNgsild/ldSubStatsFlush.h"                 // ldSubStatsFlush, ldPernotStatsFlush

#include "db/DbDriver.h"                              // db (DbDriver)
#include "db/Tenant.h"                                // tenant0, tenantList

#include "metrics/subStatsFlushAll.h"                 // Own interface



// -----------------------------------------------------------------------------
//
// dbFlushAdapter - bridges DbDriver's Tenant*-taking signature to the
// swNgsild-side generic (void*) one.
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
// subStatsFlushAll -
//
void subStatsFlushAll(void)
{
  for (Tenant* tP = &tenant0;
       tP != NULL;
       tP = (tP == &tenant0) ? tenantList : tP->next)
  {
    if (tP->subCacheP != NULL)
      ldSubStatsFlush(tP, (LdSubCache*) tP->subCacheP, dbFlushAdapter);

    if (tP->regSubCacheP != NULL)
      ldSubStatsFlush(tP, (LdSubCache*) tP->regSubCacheP, dbFlushAdapter);

    if (tP->pernotCacheP != NULL)
      ldPernotStatsFlush(tP, (LdPernotCache*) tP->pernotCacheP, dbFlushAdapter);
  }
}
