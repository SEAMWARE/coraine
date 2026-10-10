//
// FILE            snapshotTenant.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// Snap-tenant lifecycle helpers — see header.
//
#include <stdbool.h>                                     // bool
#include <stdio.h>                                       // snprintf
#include <stdlib.h>                                      // malloc, free
#include <string.h>                                      // strlen, strcpy, memset

#include "corLog/corLog.h"                               // COR_I

#include "corNgsild/LdSnapshotCache.h"                  // LdSnapshotCacheItem, ldSnapshotCacheRdLock, ldSnapshotCacheItemPin
#include "corNgsild/LdSubCache.h"                       // LdSubCache
#include "corNgsild/ldSubCache.h"                       // ldSubCacheCreate, ldSubCacheRelease
#include "corNgsild/LdPernotCache.h"                    // LdPernotCache
#include "corNgsild/ldPernotCache.h"                    // ldPernotCacheCreate, ldPernotCacheRelease
#include "db/DbDriver.h"                                 // db, DB_OK, tenantRelease
#include "troe/TroeDriver.h"                             // troe
#include "db/Tenant.h"                                   // Tenant, tenant0

#include "db/snapshotTenant.h"                           // Own interface


//
// nameSuffix - format "-_snap_<seq>" into the supplied buffer.
//
// Leading dash on the suffix matters: it lets the dbName always look
// like "<prefix>-..." so existing tenant-cleanup tooling (corDbDrop's
// "n===prefix || n.startsWith(prefix+'-')" filter) catches snap-tenant
// DBs without modification.
//
static void nameSuffix(char* buf, size_t bufLen, int snapSeq)
{
  snprintf(buf, bufLen, "-_snap_%x", snapSeq);
}



Tenant* snapshotTenantCreate(Tenant* origP, LdSnapshotCacheItem* itemP)
{
  if (origP == NULL)
    return NULL;

  int snapSeq = itemP->snapSeq;

  char suffix[32];
  nameSuffix(suffix, sizeof(suffix), snapSeq);

  Tenant* tP = (Tenant*) malloc(sizeof(Tenant));
  if (tP == NULL)
    return NULL;
  memset(tP, 0, sizeof(*tP));

  // tenant.name = origName + suffix. For the default tenant
  // (origName = "") this leaves a leading '-_snap_<seq>' which is
  // unusual but harmless — snap-tenants are never looked up by name.
  size_t origNameLen = strlen(origP->name);
  size_t suffixLen   = strlen(suffix);
  if (origNameLen + suffixLen >= sizeof(tP->name))
  {
    free(tP);
    return NULL;
  }
  memcpy(tP->name, origP->name, origNameLen);
  memcpy(tP->name + origNameLen, suffix, suffixLen);
  tP->name[origNameLen + suffixLen] = 0;

  // tenant.dbName = origDbName + suffix. Always yields a name of the
  // form "<prefix>-..." (default tenant's dbName is "<prefix>", so the
  // suffix supplies the dash).
  size_t origDbLen = strlen(origP->dbName);
  if (origDbLen + suffixLen >= sizeof(tP->dbName))
  {
    free(tP);
    return NULL;
  }
  memcpy(tP->dbName, origP->dbName, origDbLen);
  memcpy(tP->dbName + origDbLen, suffix, suffixLen);
  tP->dbName[origDbLen + suffixLen] = 0;

  tP->initialized = false;
  tP->liveTenantP = origP;
  tP->snapshotId  = itemP->id;

#if COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS
  //
  // § 7.9.2: the Core API on a snapshot includes its subscriptions - notified by the writes on the
  // snapshot, and only by them. Created with the tenant, not at its first subscription: the
  // tenant's fields are read with no lock, by requests and by the loops.
  //
  tP->subCacheP = ldSubCacheCreate();
  if (tP->subCacheP != NULL && db.geoMatchFunc != NULL)
    ((LdSubCache*) tP->subCacheP)->geoMatchFunc = db.geoMatchFunc;
  tP->pernotCacheP = ldPernotCacheCreate();
#endif
  // No regCacheP / regSubCacheP / entityMapStoreP / snapshotCacheP - a snapshot is local scope
  // (§ 7.9.2: no Context Source Registration is used) and has no snapshots of its own.

  if (db.tenantSetup != NULL)
  {
    if (db.tenantSetup(tP) != DB_OK)
    {
      snapshotTenantDestroy(tP);
      return NULL;
    }
  }
  tP->initialized = true;

  COR_I("snapshotTenant: created '%s' (db: '%s')", tP->name, tP->dbName);
  return tP;
}



void snapshotTenantDestroy(Tenant* snapTenantP)
{
  if (snapTenantP == NULL)
    return;

  //
  // Nobody is left using the caches: whatever uses them - a request routed to the snapshot, a
  // loop's visit - holds the snapshot pinned, and this runs at the last unpin.
  //
  ldSubCacheRelease((LdSubCache*) snapTenantP->subCacheP);
  ldPernotCacheRelease((LdPernotCache*) snapTenantP->pernotCacheP);

  //
  // And for the same reason the DB plugin can free what it hung on the tenant - corDB's store, which its
  // tenantDrop empties but cannot free (a request might still hold it, as far as the plugin can tell)
  //
  if (db.tenantRelease != NULL)
    db.tenantRelease(snapTenantP);

  free(snapTenantP);
}



#if COR_FEATURE_SNAPSHOT_SUBSCRIPTIONS
// -----------------------------------------------------------------------------
//
// snapshotTenantsVisit -
//
// The snapshots are picked and pinned under the cache's rdlock; the visits - notifications, DB
// queries - run with no lock, so a DELETE or a capture never waits for a send.
//
void snapshotTenantsVisit(Tenant* liveP, SnapshotTenantVisitFn visit, void* arg)
{
  LdSnapshotCache* cacheP = (LdSnapshotCache*) liveP->snapshotCacheP;

  if (cacheP == NULL)
    return;

  int                   pinnedN = 0;
  int                   cap     = 0;
  LdSnapshotCacheItem** pinnedV = NULL;

  ldSnapshotCacheRdLock(cacheP);
  for (LdSnapshotCacheItem* itemP = cacheP->head; itemP != NULL; itemP = itemP->next)
  {
    if (__atomic_load_n(&itemP->snapTenantP, __ATOMIC_ACQUIRE) == NULL)   // being created (postSnapshot / cloneSnapshot)
      continue;

    if (pinnedN == cap)
    {
      int                   newCap = (cap == 0)? 8 : 2 * cap;
      LdSnapshotCacheItem** newV   = (LdSnapshotCacheItem**) realloc(pinnedV, newCap * sizeof(LdSnapshotCacheItem*));

      if (newV == NULL)
        break;                     // the rest next time

      pinnedV = newV;
      cap     = newCap;
    }

    ldSnapshotCacheItemPin(itemP);
    pinnedV[pinnedN++] = itemP;
  }
  ldSnapshotCacheUnlock(cacheP);

  for (int ix = 0; ix < pinnedN; ix++)
  {
    visit((Tenant*) __atomic_load_n(&pinnedV[ix]->snapTenantP, __ATOMIC_ACQUIRE), arg);
    ldSnapshotCacheItemUnpin(pinnedV[ix]);          // may destroy it - deleted meanwhile
  }

  free(pinnedV);
}
#endif



// -----------------------------------------------------------------------------
//
// snapshotItemDestroy - the snapshot cache's destroy hook: the item's last reference is gone
//
// The snapshot's own stores - TRoE first, so a TRoE plugin that needs the current-state tenant
// for its cleanup still has it - and the tenant struct. It used to run in DELETE / purge, right
// after the cache item was freed, while the capture worker or a read routed to the snapshot
// could still be using that tenant - and twice for two DELETEs of the same snapshot. Now it
// runs exactly once, by whoever releases the last reference: the DELETE if nobody else holds
// the snapshot, else the worker or read that finishes last.
//
void snapshotItemDestroy(LdSnapshotCacheItem* itemP)
{
  Tenant* snapTenantP = (Tenant*) itemP->snapTenantP;

  if (snapTenantP == NULL)
    return;

  if (troe.tenantDrop != NULL)
    troe.tenantDrop(snapTenantP);
  if (db.tenantDrop != NULL)
    db.tenantDrop(snapTenantP);

  snapshotTenantDestroy(snapTenantP);
  itemP->snapTenantP = NULL;
}
