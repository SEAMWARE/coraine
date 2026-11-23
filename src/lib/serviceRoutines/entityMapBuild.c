//
// FILE            entityMapBuild.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                   // snprintf
#include <string.h>                                  // strlen, strcmp, strrchr, memset

#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corTree/corTreeBuilder.h"                  // corTreeObject, corTreeArray, corTreeString, corTreeChildAdd
#include "corRest/CorRestState.h"                    // corRest
#include "corRest/corRestOutHeader.h"                // corRestOutHeaderAdd
#include "corNgsild/corNgsild.h"                     // corNgsild
#include "corNgsild/ldQRender.h"                     // ldQRenderStored
#include "corNgsild/LdEntityMap.h"                   // LdEntityMap
#include "corNgsild/ldEntityMap.h"                   // ldEntityMapSetFilters, ldEntityMapAddEntry, ldEntityMapAddLinkedMap, ...
#include "corNgsild/ldError.h"                       // ldError
#include "corNgsild/LdProblem.h"                     // LD_ERROR_*
#include "db/Tenant.h"                               // Tenant

#include "serviceRoutines/entityMapBuild.h"          // Own interface



// -----------------------------------------------------------------------------
//
// entityMapHeader - NGSILD-EntityMap: the resource URI of the EntityMap used (TS 104-176 clause 7)
//
// On every answer an EntityMap took part in: a map the client asked for, one the broker created by
// itself, and every page served from an existing one.
//
void entityMapHeader(LdEntityMap* mapP)
{
  int   size   = strlen(mapP->mapId) + 32;
  char* mapUrl = (char*) corAlloc(&corRest.kalloc, size);

  snprintf(mapUrl, size, "/ngsi-ld/v1/entityMaps/%s", mapP->mapId);
  corRestOutHeaderAdd("NGSILD-EntityMap", mapUrl);
}



// -----------------------------------------------------------------------------
//
// entityMapFill - the matching set into the new map: its query's filters, and each entity's id with
// the sources that hold it, in the order of arrayP (creation order - sorted under orderBy)
//
void entityMapFill(LdEntityMap* mapP, CorNode* arrayP, CorNode* srcMap, CorNode* linkedMapsTracker)
{
  // Bind the query's filter params to the map (§ 9 same-parameters): a
  // later paginated reuse may re-send these with the same value or omit
  // them, but not modify or introduce one.
  ldEntityMapSetFilters(mapP, corNgsild.type, corNgsild.q, corNgsild.scopeQ,
                        corNgsild.georel, corNgsild.geometry, corNgsild.coordinates,
                        corNgsild.geoproperty);

  //
  // And q as a subscription stores it - resolved, with this request's @context, expandValues,
  // jsonKeys and langProperties applied - for a later page that omits q (LdEntityMap.boundQStored).
  //
  if (corNgsild.qExpr != NULL)
    ldEntityMapSetStoredQ(mapP, ldQRenderStored(corNgsild.qExpr, &corRest.kalloc));

  // And every URL parameter of the request but pagination - for the links and the same-parameters check (§ 9.6)
  ldEntityMapSetQueryParams(mapP);

  if (arrayP == NULL)
    return;

  //
  // Walk the array, add each entity ID to the map along with
  // its provenance: look up the entity in srcMap and flatten the
  // CorArray of source strings into a char** for ldEntityMapAddEntry.
  //
  for (CorNode* entityP = arrayP->value.head; entityP != NULL; entityP = entityP->next)
  {
    CorNode* idP = corTreeLookup(entityP, "id");
    if (idP == NULL || idP->type != CorString)
      continue;

    CorNode* srcArr = (srcMap != NULL) ? corTreeLookup(srcMap, idP->value.s) : NULL;

    int n = 0;
    if (srcArr != NULL && srcArr->type == CorArray)
      for (CorNode* s = srcArr->value.head; s != NULL; s = s->next) n++;

    if (n == 0)
    {
      // Shouldn't happen for an entity that made it into arrayP, but
      // stay safe — fall back to "@none".
      const char* lone = "@none";
      ldEntityMapAddEntry(mapP, idP->value.s, &lone, 1);
    }
    else
    {
      const char** srcV = (const char**) corAlloc(&corRest.kalloc, n * sizeof(char*));
      int i = 0;
      for (CorNode* s = srcArr->value.head; s != NULL; s = s->next)
        if (s->type == CorString)
          srcV[i++] = s->value.s;
      ldEntityMapAddEntry(mapP, idP->value.s, srcV, i);
    }
  }

  // Flush per-CSR linkedMaps tracker (§ 5.14.4.4) into the map.
  if (linkedMapsTracker != NULL)
  {
    for (CorNode* p = linkedMapsTracker->value.head; p != NULL; p = p->next)
    {
      if (p->name == NULL || p->type != CorString)
        continue;
      ldEntityMapAddLinkedMap(mapP, p->name, p->value.s);
    }
  }
}



// -----------------------------------------------------------------------------
//
// retrieveMapOpen - the EntityMap of a retrieval of one entity, into rmP (always set)
//
// TS 104-175 § 10.4.2.4: a map named by the request (NGSILD-EntityMap, or ?entityMap=<id>) that is there
// is used - "only the retrieved Entity Map shall be used to determine which Context Source Registrations
// match the Entity ID"; one that "cannot be found, or [...] has expired" is created anew, as is one asked
// for with ?entityMap=true.
//
void retrieveMapOpen(RetrieveMap* rmP, const char* entityId)
{
  memset(rmP, 0, sizeof(RetrieveMap));

  Tenant*           tP     = (Tenant*) corNgsild.tenantP;
  LdEntityMapStore* storeP = (tP != NULL) ? (LdEntityMapStore*) tP->entityMapStoreP : NULL;

  if (corNgsild.entityMapId != NULL)
  {
    LdEntityMap* mapP = (storeP != NULL) ? ldEntityMapLookupPinned(storeP, corNgsild.entityMapId) : NULL;

    if (mapP != NULL)
    {
      ldEntityMapRequestPin(mapP);     // unpinned when the response is out
      rmP->mapP   = mapP;
      rmP->entryP = ldEntityMapEntryLookup(mapP, entityId);
      rmP->linkedP = corTreeObject(corRest.kallocP, NULL);
      return;
    }

    rmP->remake = true;
  }
  else if (corNgsild.entityMapCreate == false)
    return;

  rmP->make     = true;
  rmP->sourcesP = corTreeArray(corRest.kallocP, entityId);
  rmP->linkedP  = corTreeObject(corRest.kallocP, NULL);
}



// -----------------------------------------------------------------------------
//
// retrieveMapSource - a source that answered with the entity ("@none" for the local copy), and the map it used
//
// sourceMapUrl: the source's NGSILD-EntityMap response header (NULL when none) - its last path segment is
// the map's id.
//
void retrieveMapSource(RetrieveMap* rmP, const char* sourceId, const char* sourceMapUrl)
{
  if ((rmP == NULL) || (sourceId == NULL))
    return;

  if (rmP->sourcesP != NULL)
  {
    bool known = false;

    for (CorNode* nodeP = rmP->sourcesP->value.head; nodeP != NULL; nodeP = nodeP->next)
    {
      if (strcmp(nodeP->value.s, sourceId) == 0)
      {
        known = true;
        break;
      }
    }

    if (known == false)
      corTreeChildAdd(rmP->sourcesP, corTreeString(corRest.kallocP, NULL, sourceId));
  }

  if ((rmP->linkedP != NULL) && (sourceMapUrl != NULL) && (sourceMapUrl[0] != 0) && (strcmp(sourceId, "@none") != 0))
  {
    const char* slash = strrchr(sourceMapUrl, '/');
    const char* mapId = ((slash != NULL) && (slash[1] != 0)) ? slash + 1 : sourceMapUrl;

    corTreeChildAdd(rmP->linkedP, corTreeString(corRest.kallocP, sourceId, mapId));
  }
}



// -----------------------------------------------------------------------------
//
// retrieveMapClose - the retrieval succeeded: the map made (201) or the one used (200), in NGSILD-EntityMap
//
// TS 104-176 table 7.3.3.1-2: "201 Created (in case an EntityMap has been (re)created)". The map made holds
// the entity, with the sources that answered with it, and the maps they used; ?entityMapLifetime= is its
// lifetime when ?entityMap=true asked for it (TS 104-176: "Only applicable if entityMap is set to true"),
// one made in place of a map that is gone has the broker's default. The map used gets the maps its
// sources used this time (§ 10.4.2.4: "the Entity Map's linked maps are updated").
//
bool retrieveMapClose(RetrieveMap* rmP, const char* entityId)
{
  Tenant*           tP     = (Tenant*) corNgsild.tenantP;
  LdEntityMapStore* storeP = (tP != NULL) ? (LdEntityMapStore*) tP->entityMapStoreP : NULL;

  if (rmP->mapP != NULL)
  {
    for (CorNode* linkP = rmP->linkedP->value.head; linkP != NULL; linkP = linkP->next)
      ldEntityMapLinkedMapSet(storeP, rmP->mapP, linkP->name, linkP->value.s);

    ldEntityMapTouch(rmP->mapP);
    entityMapHeader(rmP->mapP);
    return false;
  }

  if ((rmP->make == false) || (storeP == NULL))
    return false;

  ldEntityMapPurgeExpired(storeP);

  LdEntityMap* mapP = ldEntityMapCreate(storeP, rmP->remake ? 0 : corNgsild.entityMapLifetimeNs, tP);

  if (mapP == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error", "out of memory creating an EntityMap");
    return true;
  }

  mapP->automatic = rmP->remake;
  ldEntityMapRequestPin(mapP);   // created pinned; held (and filled, rendered) till the request ends

  CorNode* arrayP  = corTreeArray(corRest.kallocP, NULL);
  CorNode* entityP = corTreeObject(corRest.kallocP, NULL);
  CorNode* srcMap  = corTreeObject(corRest.kallocP, NULL);

  corTreeChildAdd(entityP, corTreeString(corRest.kallocP, "id", entityId));
  corTreeChildAdd(arrayP, entityP);
  corTreeChildAdd(srcMap, rmP->sourcesP);

  entityMapFill(mapP, arrayP, srcMap, rmP->linkedP);

  if (ldEntityMapAdmit(storeP, mapP) == false)
  {
    ldError(403, LD_ERROR_TOO_MANY_RESULTS, "Too Many Results",
            "the EntityMap does not fit in the memory of the EntityMaps (--entityMapMemory %lld MiB)",
            (long long) (ldEntityMapMaxBytes >> 20));
    return true;
  }

  entityMapHeader(mapP);

  corNgsild.entityMapCreate  = true;     // the 201's body is the entity, its @context advertised as a query's
  corRest.out.httpStatusCode = 201;

  return false;
}
