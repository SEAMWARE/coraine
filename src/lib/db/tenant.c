//
// FILE            tenant.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
#include <ctype.h>                                       // tolower
#include <stdlib.h>                                      // malloc
#include <string.h>                                      // strcmp, strncpy, snprintf
#include <pthread.h>                                     // pthread_mutex_t

#include "ktrace/kTrace.h"                               // KT_I
#include "swRest/SwRestState.h"                          // swRest
#include "swNgsild/SwNgsild.h"                           // swNgsild
#include "swNgsild/ldExpandParams.h"                     // ldExpandParams
#include "swNgsild/ldParamsValidate.h"                   // ldParamsValidate
#include "swNgsild/ldError.h"                            // ldError
#include "swNgsild/LdProblem.h"                          // LD_ERROR_NONEXISTENT_TENANT

#include "swNgsild/LdSubCache.h"                         // LdSubCache
#include "swNgsild/ldSubCache.h"                         // ldSubCacheCreate, ldSubCacheItemAdd
#include "swNgsild/LdPernotCache.h"                      // LdPernotCache
#include "swNgsild/ldPernotCache.h"                      // ldPernotCacheCreate
#include "swNgsild/LdEntityMap.h"                         // LdEntityMapStore
#include "swNgsild/ldEntityMap.h"                         // ldEntityMapStoreCreate
#include "swNgsild/LdSnapshotCache.h"                     // ldSnapshotCacheCreate, ldSnapshotCacheItemAdd, ldSnapshotCacheItemDelete
#include "db/snapshotTenant.h"                            // snapshotTenantCreate, snapshotTenantDestroy
#include "swNgsild/LdRegCache.h"                          // LdRegCache
#include "swNgsild/ldRegCache.h"                         // ldRegCacheCreate, ldRegCacheItemAdd
#include "kjson/kjLookup.h"                              // kjLookup

#include "db/DbDriver.h"                                // db
#include "db/Tenant.h"                                   // Own interface



// -----------------------------------------------------------------------------
//
// Module state
//
static char     dbPrefix[128];
Tenant          tenant0;
Tenant*         tenantList = NULL;

// Serialises tenant CREATION - see tenantGetOrCreate
static pthread_mutex_t tenantMutex = PTHREAD_MUTEX_INITIALIZER;



// -----------------------------------------------------------------------------
//
// tenantInit -
//
void tenantInit(const char* prefix)
{
  strncpy(dbPrefix, prefix, sizeof(dbPrefix) - 1);
  dbPrefix[sizeof(dbPrefix) - 1] = 0;

  memset(&tenant0, 0, sizeof(Tenant));
  tenant0.name[0]     = 0;
  strncpy(tenant0.dbName, prefix, sizeof(tenant0.dbName) - 1);
  tenant0.dbName[sizeof(tenant0.dbName) - 1] = 0;
  tenant0.initialized = true;
  tenant0.subCacheP    = ldSubCacheCreate();
  if (tenant0.subCacheP != NULL && db.geoMatchFunc != NULL)
    ((LdSubCache*) tenant0.subCacheP)->geoMatchFunc = db.geoMatchFunc;
  tenant0.pernotCacheP    = ldPernotCacheCreate();
  tenant0.regCacheP       = ldRegCacheCreate();
  if (tenant0.regCacheP != NULL && db.csrGeoMatchFunc != NULL)
    ((LdRegCache*) tenant0.regCacheP)->csrGeoMatchFunc = db.csrGeoMatchFunc;
  tenant0.regSubCacheP    = ldSubCacheCreate();
  tenant0.entityMapStoreP = ldEntityMapStoreCreate();
  tenant0.snapshotCacheP  = ldSnapshotCacheCreate();
  tenant0.next        = NULL;
}



// -----------------------------------------------------------------------------
//
// tenantDbPrefixSet - point the default tenant at the configured DB name
//
// main runs tenantInit() before any DB plugin starts, creating tenant0 and its
// caches with a placeholder prefix. The DB plugin's init then calls this with
// the real database name — binding the default tenant and the multi-tenant
// db-name prefix WITHOUT re-creating the caches (re-running tenantInit would
// orphan them: a startup leak).
//
void tenantDbPrefixSet(const char* prefix)
{
  strncpy(dbPrefix, prefix, sizeof(dbPrefix) - 1);
  dbPrefix[sizeof(dbPrefix) - 1] = 0;

  strncpy(tenant0.dbName, prefix, sizeof(tenant0.dbName) - 1);
  tenant0.dbName[sizeof(tenant0.dbName) - 1] = 0;
}



// -----------------------------------------------------------------------------
//
// tenantLookup - find a tenant by name
//
Tenant* tenantLookup(const char* name)
{
  if (name == NULL || name[0] == 0)
    return &tenant0;

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
  {
    if (strcasecmp(tP->name, name) == 0)
      return tP;
  }

  return NULL;
}



// -----------------------------------------------------------------------------
//
// tenantGetOrCreate - find or allocate a new tenant
//
// Double-checked under tenantMutex: the tenant list is prepended to here and
// walked lock-free by every request. Two threads creating the same tenant at the
// same time would otherwise end up with two Tenant structs - two sets of caches,
// only one of them reachable - and a request landing on the losing one would
// find its subscriptions and registrations missing. The HA channel is one such
// thread: a tenant another instance invented is created from its thread.
//
// The lock covers the CREATE only. A reader walking the list concurrently is
// safe as it stands: a new tenant is fully built before it becomes the head, and
// a tenant is never removed from the list.
//
Tenant* tenantGetOrCreate(const char* name)
{
  Tenant* tP = tenantLookup(name);

  if (tP != NULL)
    return tP;

  pthread_mutex_lock(&tenantMutex);

  // Somebody else may have created it while we were waiting for the lock
  tP = tenantLookup(name);

  if (tP != NULL)
  {
    pthread_mutex_unlock(&tenantMutex);
    return tP;
  }

  tP = (Tenant*) malloc(sizeof(Tenant));
  if (tP == NULL)
  {
    pthread_mutex_unlock(&tenantMutex);
    return NULL;
  }

  memset(tP, 0, sizeof(Tenant));

  // Validate name length: must fit in name[] and dbName[] ("prefix-name")
  int nameLen   = strlen(name);
  int prefixLen = strlen(dbPrefix);

  if (nameLen >= (int) sizeof(tP->name) || prefixLen + 1 + nameLen >= (int) sizeof(tP->dbName))
  {
    free(tP);
    pthread_mutex_unlock(&tenantMutex);
    return NULL;
  }

  // Lowercase the name
  for (int i = 0; i < nameLen; i++)
    tP->name[i] = tolower(name[i]);
  tP->name[nameLen] = 0;

  // Build DB name: "prefix-tenantname"  (length already validated above)
  strcpy(tP->dbName, dbPrefix);
  strcat(tP->dbName, "-");
  strcat(tP->dbName, tP->name);

  tP->initialized = false;
  tP->subCacheP    = ldSubCacheCreate();
  if (tP->subCacheP != NULL && db.geoMatchFunc != NULL)
    ((LdSubCache*) tP->subCacheP)->geoMatchFunc = db.geoMatchFunc;
  tP->pernotCacheP    = ldPernotCacheCreate();
  tP->regCacheP       = ldRegCacheCreate();
  if (tP->regCacheP != NULL && db.csrGeoMatchFunc != NULL)
    ((LdRegCache*) tP->regCacheP)->csrGeoMatchFunc = db.csrGeoMatchFunc;
  tP->regSubCacheP    = ldSubCacheCreate();
  tP->entityMapStoreP = ldEntityMapStoreCreate();
  tP->snapshotCacheP  = ldSnapshotCacheCreate();

  // Prepend to linked list
  tP->next   = tenantList;
  tenantList = tP;

  KT_I("tenant: created tenant '%s' (db: '%s')", tP->name, tP->dbName);

  pthread_mutex_unlock(&tenantMutex);

  return tP;
}



// -----------------------------------------------------------------------------
//
// tenantFromRequest - resolve tenant from NGSILD-Tenant request header
//
Tenant* tenantFromRequest(bool autoCreate)
{
  //
  // Search for NGSILD-Tenant header in the request
  //
  const char* tenantName = NULL;

  for (int i = 0; i < swRest.in.httpHeaderCount; i++)
  {
    if (strcasecmp(swRest.in.httpHeaderV[i].key, "NGSILD-Tenant") == 0)
    {
      tenantName = swRest.in.httpHeaderV[i].value;
      break;
    }
  }

  //
  // No tenant header -- use default tenant
  //
  if (tenantName == NULL || tenantName[0] == 0)
    return &tenant0;

  //
  // Look up existing tenant
  //
  Tenant* tP = tenantLookup(tenantName);

  if (tP != NULL)
  {
    // Echo tenant header in response (NGSI-LD spec 6.3.14)
    SwRestKeyValue* hV = swRest.out.headerV;
    int ix = swRest.out.headerCount;
    hV[ix].key   = "NGSILD-Tenant";
    hV[ix].value = tP->name;
    swRest.out.headerCount++;
    return tP;
  }

  //
  // Tenant not found -- auto-create for write operations, 404 for read operations
  //
  if (!autoCreate)
  {
    ldError(404, LD_ERROR_NONEXISTENT_TENANT, "NonexistentTenant", "tenant '%s' does not exist", tenantName);
    return NULL;
  }

  //
  // Auto-create: allocate tenant, set up DB (indexes), mark initialized
  //
  tP = tenantGetOrCreate(tenantName);
  if (tP == NULL)
  {
    ldError(400, LD_ERROR_BAD_REQUEST_DATA, "Invalid Field Value", "tenant name too long: '%s'", tenantName);
    return NULL;
  }

  if (!tP->initialized && db.tenantSetup != NULL)
  {
    db.tenantSetup(tP);
    tP->initialized = true;
  }

  // Echo tenant header in response
  SwRestKeyValue* hV = swRest.out.headerV;
  int ix = swRest.out.headerCount;
  hV[ix].key   = "NGSILD-Tenant";
  hV[ix].value = tP->name;
  swRest.out.headerCount++;

  return tP;
}



// -----------------------------------------------------------------------------
//
// tenantPreServiceHook - resolve tenant before every service routine
//
// Uses the HTTP verb to decide: POST/PATCH/DELETE auto-create, GET rejects unknown.
// Stores the result in swNgsild.tenantP.
// Returns true to continue to service routine, false to skip (error already set).
//
bool tenantPreServiceHook(void)
{
  //
  // /info/sourceIdentity is tenant-agnostic at the broker level — the
  // alias it returns is derived from the NGSILD-Tenant header, whether
  // or not that tenant has been created on this broker. Bypass the
  // tenant lookup so unknown tenants still get a valid probe response.
  //
  if (swRest.in.urlPath != NULL && strcmp(swRest.in.urlPath, "/ngsi-ld/v1/info/sourceIdentity") == 0)
    return true;

  bool autoCreate = (swRest.in.verb != SwVerbGet);

  Tenant* tP = tenantFromRequest(autoCreate);

  if (tP == NULL)
    return false;

  swNgsild.tenantP = tP;

  // Expand vocab-bearing URL params (type, pick, omit, etc.) now that
  // @context is resolved and all params are parsed.
  ldExpandParams(&swRest.kalloc);

  // Cross-parameter sanity (pick/omit conflicts, attrs+pick mutex,
  // geo-query shape, etc.). Hooking here — rather than only inside
  // each entity-query service routine — also covers routes that take
  // geo URL params but don't run the per-route validator (e.g.
  // csourceRegistrations / csourceSubscriptions): a malformed Polygon
  // on those endpoints is now caught up front instead of sliding
  // through to the geo matcher.
  if (ldParamsValidate())
    return false;

  return true;
}



// -----------------------------------------------------------------------------
//
// tenantSubCacheItemKind - which of the three sub caches owns this document?
//
// All three kinds live in ONE collection: entity subscriptions, periodic ones
// (a numeric timeInterval) and CSR-subs (§ 5.11), the last tagged _subKind="csr"
// at insert. Deciding it in one place is what keeps the startup load and the HA
// apply from drifting — two copies of this routing is how a subscription ends up
// cached twice, or in the cache that never looks at it.
//
static int tenantSubCacheItemKind(KjNode* subP)
{
  KjNode* kindP = kjLookup(subP, "_subKind");

  if (kindP != NULL && kindP->type == KjString && strcmp(kindP->value.s, "csr") == 0)
    return TENANT_SUB_KIND_CSR;

  KjNode* tiP = kjLookup(subP, "timeInterval");

  if (tiP != NULL && (tiP->type == KjInt || tiP->type == KjFloat))
    return TENANT_SUB_KIND_PERNOT;

  return TENANT_SUB_KIND_ENTITY;
}



// -----------------------------------------------------------------------------
//
// tenantSubCacheItemIdGet - the document's id, whichever name it carries
//
static const char* tenantSubCacheItemIdGet(KjNode* subP)
{
  KjNode* idP = kjLookup(subP, "id");

  if ((idP == NULL) || (idP->type != KjString))
    idP = kjLookup(subP, "_id");

  return ((idP != NULL) && (idP->type == KjString))? idP->value.s : NULL;
}



// -----------------------------------------------------------------------------
//
// tenantSubCacheItemStore - put one subscription document in the cache that owns it
//
// 'replace' is for the callers that may be overwriting a subscription already
// cached (the HA apply); the startup load knows the cache is empty and skips the
// lookup, which would otherwise make loading N subscriptions O(N²).
//
// Returns the TENANT_SUB_KIND_* the document was routed to, or
// TENANT_SUB_KIND_NONE if the cache it belongs in does not exist.
//
int tenantSubCacheItemStore(Tenant* tP, KjNode* subP, bool replace)
{
  int         kind  = tenantSubCacheItemKind(subP);
  const char* subId = tenantSubCacheItemIdGet(subP);

  //
  // A subscription can MOVE between caches - a PATCH that adds or removes
  // timeInterval makes a periodic subscription of an entity one, and back. The
  // copy in the cache it left has to go, or it keeps notifying on its own terms.
  //
  if (replace && (subId != NULL))
  {
    if ((kind != TENANT_SUB_KIND_PERNOT) && (tP->pernotCacheP != NULL))
      ldPernotCacheItemRemove((LdPernotCache*) tP->pernotCacheP, subId);

    if ((kind != TENANT_SUB_KIND_ENTITY) && (tP->subCacheP != NULL))
    {
      ldSubCacheWrLock((LdSubCache*) tP->subCacheP);
      ldSubCacheItemRemove((LdSubCache*) tP->subCacheP, subId);
      ldSubCacheUnlock((LdSubCache*) tP->subCacheP);
    }

    if ((kind != TENANT_SUB_KIND_CSR) && (tP->regSubCacheP != NULL))
    {
      ldSubCacheWrLock((LdSubCache*) tP->regSubCacheP);
      ldSubCacheItemRemove((LdSubCache*) tP->regSubCacheP, subId);
      ldSubCacheUnlock((LdSubCache*) tP->regSubCacheP);
    }
  }

  if (kind == TENANT_SUB_KIND_PERNOT)
  {
    if (tP->pernotCacheP == NULL)
      return TENANT_SUB_KIND_NONE;

    if (replace && (subId != NULL))
      ldPernotCacheItemRemove((LdPernotCache*) tP->pernotCacheP, subId);

    ldPernotCacheItemAdd((LdPernotCache*) tP->pernotCacheP, subP, NULL, tP);
    return TENANT_SUB_KIND_PERNOT;
  }

  LdSubCache* cacheP = (LdSubCache*) ((kind == TENANT_SUB_KIND_CSR)? tP->regSubCacheP : tP->subCacheP);

  if (cacheP == NULL)
    return TENANT_SUB_KIND_NONE;

  //
  // One lock hold for remove+add: a reader walking the cache in between would
  // find the subscription missing and skip a notification it should have sent.
  //
  ldSubCacheWrLock(cacheP);

  LdSubSubordinate* savedSubordinateP     = NULL;
  int               savedSubordinateRunNo = 0;
  int               unflushedSent         = 0;
  int               unflushedFailed       = 0;

  if (replace && (subId != NULL))
  {
    LdSubCacheItem* oldP = ldSubCacheItemLookup(cacheP, subId);

    if (oldP != NULL)
    {
      //
      // Two things the stored document cannot tell us, both lost if the item
      // is simply thrown away:
      //
      //   o the derived subscriptions this broker has on remote Context
      //     Sources (§ 5.8.1.4). The live mapping is the authoritative one -
      //     the same rule the local PATCH path follows - so it is carried
      //     over unless the document brings one of its own.
      //   o the notifications sent since the last stats flush. They are
      //     counted as a delta against lastFlushed*, so dropping them means
      //     they are never $inc'd into the database at all.
      //
      savedSubordinateP     = oldP->subordinateP;
      savedSubordinateRunNo = oldP->subordinateRunNo;
      oldP->subordinateP    = NULL;   // detached: the item release must not free it

      unflushedSent   = oldP->timesSent   - oldP->lastFlushedSent;
      unflushedFailed = oldP->timesFailed - oldP->lastFlushedFailed;

      ldSubCacheItemRemove(cacheP, subId);
    }
  }

  LdSubCacheItem* newP = ldSubCacheItemAdd(cacheP, subP, NULL, LdFormatUnset);

  if (newP != NULL)
  {
    if (savedSubordinateP != NULL)
    {
      ldSubCacheSubordinatesFree(newP->subordinateP);
      newP->subordinateP     = savedSubordinateP;
      newP->subordinateRunNo = savedSubordinateRunNo;
      savedSubordinateP      = NULL;   // ownership transferred
    }

    // lastFlushed* stays as read from the document, so this is exactly what the
    // next flush will $inc.
    newP->timesSent   += unflushedSent;
    newP->timesFailed += unflushedFailed;
  }

  ldSubCacheUnlock(cacheP);

  // The add bailed out - the detached mapping has no owner left to free it.
  if (savedSubordinateP != NULL)
    ldSubCacheSubordinatesFree(savedSubordinateP);

  return kind;
}



// -----------------------------------------------------------------------------
//
// tenantSubCacheItemDrop - remove a subscription from whichever cache holds it
//
// The kind cannot be derived here - the document is gone. All three are tried,
// and all three are tried even after a hit: a PATCH that added or removed
// timeInterval moves a subscription between caches, so an id in two of them is
// not impossible, and leaving a stale copy behind would keep notifying.
//
bool tenantSubCacheItemDrop(Tenant* tP, const char* subId)
{
  bool removed = false;

  if (tP->subCacheP != NULL)
  {
    ldSubCacheWrLock((LdSubCache*) tP->subCacheP);
    removed |= ldSubCacheItemRemove((LdSubCache*) tP->subCacheP, subId);
    ldSubCacheUnlock((LdSubCache*) tP->subCacheP);
  }

  if (tP->regSubCacheP != NULL)
  {
    ldSubCacheWrLock((LdSubCache*) tP->regSubCacheP);
    removed |= ldSubCacheItemRemove((LdSubCache*) tP->regSubCacheP, subId);
    ldSubCacheUnlock((LdSubCache*) tP->regSubCacheP);
  }

  if (tP->pernotCacheP != NULL)
    removed |= ldPernotCacheItemRemove((LdPernotCache*) tP->pernotCacheP, subId);

  return removed;
}



// -----------------------------------------------------------------------------
//
// tenantSubCacheLoad - load subscriptions for a single tenant
//
static void tenantSubCacheLoad(Tenant* tP)
{
  if (db.subscriptionQuery == NULL)
    return;

  KjNode* arrayP = NULL;
  int     r      = db.subscriptionQuery(tP, 0, 0, &arrayP);

  if (r != DB_OK || arrayP == NULL)
    return;

  int normalCount = 0;
  int pernotCount = 0;
  int csrCount    = 0;

  for (KjNode* subP = arrayP->value.firstChildP; subP != NULL; subP = subP->next)
  {
    switch (tenantSubCacheItemStore(tP, subP, false))
    {
    case TENANT_SUB_KIND_ENTITY:  normalCount++;  break;
    case TENANT_SUB_KIND_PERNOT:  pernotCount++;  break;
    case TENANT_SUB_KIND_CSR:     csrCount++;     break;
    }
  }

  if (normalCount + pernotCount + csrCount > 0)
    KT_I("tenant '%s': loaded %d subscription(s) + %d pernot + %d csr-sub into cache",
         tP->name[0] ? tP->name : "(default)", normalCount, pernotCount, csrCount);
}



// -----------------------------------------------------------------------------
//
// tenantSubCacheReload -
//
void tenantSubCacheReload(void)
{
  // Default tenant
  tenantSubCacheLoad(&tenant0);

  // All other tenants
  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
    tenantSubCacheLoad(tP);
}



// -----------------------------------------------------------------------------
//
// tenantRegCacheLoad - load registrations for a single tenant
//
static void tenantRegCacheLoad(Tenant* tP)
{
  if (tP->regCacheP == NULL || db.registrationQuery == NULL)
    return;

  KjNode* arrayP = NULL;
  int     r      = db.registrationQuery(tP, 0, 0, &arrayP);

  if (r != DB_OK || arrayP == NULL)
    return;

  int count = 0;
  for (KjNode* regP = arrayP->value.firstChildP; regP != NULL; regP = regP->next)
  {
    // swRest.kalloc is the startup buffer here (swBroker main reset it right
    // after the cache reloads) — fine as the transient arena for resolving a
    // CSR's forwarding @context.
    ldRegCacheItemAdd((LdRegCache*) tP->regCacheP, regP, &swRest.kalloc);
    count++;
  }

  if (count > 0)
    KT_I("tenant '%s': loaded %d registration(s) into cache", tP->name[0] ? tP->name : "(default)", count);
}



// -----------------------------------------------------------------------------
//
// tenantRegCacheReload -
//
void tenantRegCacheReload(void)
{
  tenantRegCacheLoad(&tenant0);

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
    tenantRegCacheLoad(tP);
}



// -----------------------------------------------------------------------------
//
// tenantSubCacheItemRefresh - re-read one subscription from the DB into the cache
//
// For a caller that knows only that the row changed - the HA apply, told so by
// another broker instance. Reading it back and running it through the very
// function the startup load uses is what makes a subscription that arrived over
// HA identical to one created locally; a second, hand-written conversion here is
// how the two copies would drift.
//
// A row that is gone is not an error: a create and a delete can both have
// happened before either was applied. Dropping the cached copy is then exactly
// right - it is what the (already queued) delete would do anyway, and doing it
// now means the cache is never left holding a subscription the database does not
// have.
//
bool tenantSubCacheItemRefresh(Tenant* tP, const char* subId)
{
  if (db.subscriptionRetrieve == NULL)
    return false;

  KjNode* subP = NULL;
  int     r    = db.subscriptionRetrieve(tP, subId, &subP);

  if ((r == DB_NOT_FOUND) || ((r == DB_OK) && (subP == NULL)))
  {
    tenantSubCacheItemDrop(tP, subId);
    return true;
  }

  if (r != DB_OK)
    return false;

  return (tenantSubCacheItemStore(tP, subP, true) != TENANT_SUB_KIND_NONE);
}



// -----------------------------------------------------------------------------
//
// tenantRegCacheItemDrop - remove a registration from the cache
//
bool tenantRegCacheItemDrop(Tenant* tP, const char* regId)
{
  if (tP->regCacheP == NULL)
    return false;

  ldRegCacheWrLock((LdRegCache*) tP->regCacheP);
  bool removed = ldRegCacheItemRemove((LdRegCache*) tP->regCacheP, regId);
  ldRegCacheUnlock((LdRegCache*) tP->regCacheP);

  return removed;
}



// -----------------------------------------------------------------------------
//
// tenantRegCacheItemRefresh - re-read one registration from the DB into the cache
//
// Same shape as the subscription refresh. The reg cache has no update-in-place,
// so the old item must go first - a registration cached twice is matched twice,
// and the same request is forwarded twice to the same Context Source.
//
bool tenantRegCacheItemRefresh(Tenant* tP, const char* regId)
{
  if ((tP->regCacheP == NULL) || (db.registrationRetrieve == NULL))
    return false;

  KjNode* regP = NULL;
  int     r    = db.registrationRetrieve(tP, regId, &regP);

  if ((r == DB_NOT_FOUND) || ((r == DB_OK) && (regP == NULL)))
  {
    tenantRegCacheItemDrop(tP, regId);
    return true;
  }

  if (r != DB_OK)
    return false;

  ldRegCacheWrLock((LdRegCache*) tP->regCacheP);
  ldRegCacheItemRemove((LdRegCache*) tP->regCacheP, regId);
  ldRegCacheItemAdd((LdRegCache*) tP->regCacheP, regP, &swRest.kalloc);
  ldRegCacheUnlock((LdRegCache*) tP->regCacheP);

  return true;
}



// -----------------------------------------------------------------------------
//
// tenantSnapshotCacheLoad - load persisted snapshots for one tenant.
//
// Each persisted snapshot has a hidden "_snapSeq" field — used to
// rebuild the snap-tenant. Without it the entity store can't be
// located after restart, so the snapshot is skipped (and logged).
//
static void tenantSnapshotCacheLoad(Tenant* tP)
{
  if (tP->snapshotCacheP == NULL || db.snapshotQuery == NULL)
    return;

  KjNode* arrayP = NULL;
  int     r      = db.snapshotQuery(tP, &arrayP);
  if (r != DB_OK || arrayP == NULL)
    return;

  LdSnapshotCache* cacheP = (LdSnapshotCache*) tP->snapshotCacheP;

  int  count   = 0;
  int  maxSeq  = -1;

  for (KjNode* snapP = arrayP->value.firstChildP; snapP != NULL; snapP = snapP->next)
  {
    KjNode* seqP = kjLookup(snapP, "_snapSeq");
    if (seqP == NULL || (seqP->type != KjInt && seqP->type != KjFloat))
    {
      KT_E("tenant '%s': persisted snapshot missing _snapSeq — skipped", tP->name[0] ? tP->name : "(default)");
      continue;
    }
    int snapSeq = (seqP->type == KjInt) ? (int) seqP->value.i : (int) seqP->value.f;

    // ldSnapshotCacheItemAdd assigns its own snapSeq from cacheP->nextSnapSeq;
    // override that with the persisted one so the snap-tenant DB name matches
    // what's already on disk. We do that by bumping nextSnapSeq right before.
    int saved = cacheP->nextSnapSeq;
    cacheP->nextSnapSeq = snapSeq;

    LdSnapshotCacheItem* itemP = ldSnapshotCacheItemAdd(cacheP, snapP);

    cacheP->nextSnapSeq = saved;

    if (itemP == NULL)
    {
      KT_E("tenant '%s': failed to add persisted snapshot to cache", tP->name[0] ? tP->name : "(default)");
      continue;
    }

    itemP->snapSeq     = snapSeq;
    itemP->snapTenantP = snapshotTenantCreate(tP, snapSeq);
    if (itemP->snapTenantP == NULL)
    {
      KT_E("tenant '%s': snapshot '%s': failed to reconstruct snap-tenant",
           tP->name[0] ? tP->name : "(default)", itemP->id);
      ldSnapshotCacheItemDelete(cacheP, itemP->id);
      continue;
    }

    if (snapSeq > maxSeq)
      maxSeq = snapSeq;
    count++;
  }

  if (maxSeq >= 0 && cacheP->nextSnapSeq <= maxSeq)
    cacheP->nextSnapSeq = maxSeq + 1;

  if (count > 0)
    KT_I("tenant '%s': loaded %d snapshot(s) into cache (nextSnapSeq=%d)",
         tP->name[0] ? tP->name : "(default)", count, cacheP->nextSnapSeq);
}



// -----------------------------------------------------------------------------
//
// tenantSnapshotCacheReload -
//
void tenantSnapshotCacheReload(void)
{
  tenantSnapshotCacheLoad(&tenant0);

  for (Tenant* tP = tenantList; tP != NULL; tP = tP->next)
    tenantSnapshotCacheLoad(tP);
}
