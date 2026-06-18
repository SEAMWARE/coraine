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
Tenant* tenantGetOrCreate(const char* name)
{
  Tenant* tP = tenantLookup(name);

  if (tP != NULL)
    return tP;

  tP = (Tenant*) malloc(sizeof(Tenant));
  if (tP == NULL)
    return NULL;

  memset(tP, 0, sizeof(Tenant));

  // Validate name length: must fit in name[] and dbName[] ("prefix-name")
  int nameLen   = strlen(name);
  int prefixLen = strlen(dbPrefix);

  if (nameLen >= (int) sizeof(tP->name) || prefixLen + 1 + nameLen >= (int) sizeof(tP->dbName))
  {
    free(tP);
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
  if (swRest.in.urlPath != NULL && strcmp(swRest.in.urlPath, "/info/sourceIdentity") == 0)
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
    // CSR-subs (§ 5.11) are persisted in the same collection, tagged
    // with _subKind="csr". Route them to regSubCacheP and skip the
    // entity-sub paths.
    KjNode* kindP = kjLookup(subP, "_subKind");
    bool    isCsr = (kindP != NULL && kindP->type == KjString && strcmp(kindP->value.s, "csr") == 0);

    if (isCsr && tP->regSubCacheP != NULL)
    {
      ldSubCacheItemAdd((LdSubCache*) tP->regSubCacheP, subP, NULL);
      csrCount++;
      continue;
    }

    KjNode* tiP = kjLookup(subP, "timeInterval");
    bool isPernot = (tiP != NULL && (tiP->type == KjInt || tiP->type == KjFloat));

    if (isPernot && tP->pernotCacheP != NULL)
    {
      ldPernotCacheItemAdd((LdPernotCache*) tP->pernotCacheP, subP, NULL, tP);
      pernotCount++;
    }
    else if (!isPernot && tP->subCacheP != NULL)
    {
      ldSubCacheItemAdd((LdSubCache*) tP->subCacheP, subP, NULL);
      normalCount++;
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
    ldRegCacheItemAdd((LdRegCache*) tP->regCacheP, regP);
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
