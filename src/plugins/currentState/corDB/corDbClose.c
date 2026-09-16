//
// FILE            corDbClose.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <pthread.h>                                     // pthread_rwlock_destroy
#include <stdlib.h>                                      // free

#include "ktrace/kTrace.h"                               // KT_I

#include "khash/khash.h"                               // khashRelease
#include "kjson/kjFree.h"                                // kjFree

#include "db/Tenant.h"                                   // tenant0, tenantList
#include "currentState/corDB/corDbGeoMatch.h"          // corDbGeoClose
#include "currentState/corDB/corDbStore.h"        // CorDbStore
#include "currentState/corDB/corDbClose.h"             // Own interface



// -----------------------------------------------------------------------------
//
// corDbFreeTenantStore - free the per-tenant KjNode tree
//
static void corDbFreeTenantStore(Tenant* tenantP)
{
  if (tenantP->pluginData != NULL)
  {
    CorDbStore* storeP = (CorDbStore*) tenantP->pluginData;

    //
    // The tree first, then the lock, then the struct that holds both. Nothing
    // else is running by now - corDbClose is called once, at shutdown, after
    // the HTTP server has stopped - so there is no last writer to wait for.
    //
    if (storeP->idIndex != NULL)
      khashRelease(storeP->idIndex);

    kjFree(storeP->tree);
    pthread_rwlock_destroy(&storeP->lock);
    free(storeP);

    tenantP->pluginData = NULL;
  }
}



// -----------------------------------------------------------------------------
//
// corDbClose -
//
void corDbClose(void)
{
  corDbFreeTenantStore(&tenant0);

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
    corDbFreeTenantStore(tP);

  corDbGeoClose();
  KT_I("corDB: closed (all tenant stores freed)");
}
