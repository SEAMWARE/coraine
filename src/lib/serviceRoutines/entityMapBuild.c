//
// FILE            entityMapBuild.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
#include <stdio.h>                                   // snprintf
#include <string.h>                                  // strlen

#include "corAlloc/corAlloc.h"                       // corAlloc
#include "corTree/CorNode.h"                         // CorNode
#include "corTree/corTreeLookup.h"                   // corTreeLookup
#include "corRest/CorRestState.h"                    // corRest
#include "corRest/corRestOutHeader.h"                // corRestOutHeaderAdd
#include "corNgsild/corNgsild.h"                     // corNgsild
#include "corNgsild/ldQRender.h"                     // ldQRenderStored
#include "corNgsild/LdEntityMap.h"                   // LdEntityMap
#include "corNgsild/ldEntityMap.h"                   // ldEntityMapSetFilters, ldEntityMapAddEntry, ldEntityMapAddLinkedMap

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
