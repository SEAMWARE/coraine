//
// FILE            getType.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/types/{type} — Retrieve Available Entity Type Information
// (§ 5.7.7). Returns EntityTypeInfo (§ 5.2.26): the type, its entity count
// and the attributeDetails list (Attributes restricted to id, type,
// attributeName, attributeTypes).
//
// Mode 1 only (local). Modes 2/3 to follow.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                       // corRest
#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corJsonld/corLdCompact.h"                     // corLdCompact
#include "corJsonld/corLdExpand.h"                      // corLdExpand
#include "corJsonld/corLdInit.h"                        // corLdCoreContext

#include "corNgsild/corNgsild.h"                        // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdRegCache.h"                      // LdRegCache
#include "corNgsild/ldDiscovery.h"                     // ldDiscoveryRegAugmentTypes
#include "corNgsild/ldDiscoveryForward.h"              // ldDiscoveryForwardType, ldDiscoveryShouldForward
#include "corNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/getType.h"                  // Own interface



// -----------------------------------------------------------------------------
//
// shortOrSelf -
//
static const char* shortOrSelf(CorLdContext* ctxP, const char* iri)
{
  const char* compact = corLdCompact(ctxP, iri);
  return (compact != NULL) ? compact : iri;
}



// -----------------------------------------------------------------------------
//
// getType -
//
bool getType(void)
{
  Tenant*     tenantP    = (Tenant*) corNgsild.tenantP;
  const char* typeWild   = corRest.in.wildcard[0];    // url-decoded already

  if (db.typeList == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "type discovery not supported by this DB plugin");
    return true;
  }

  CorLdContext* ctxP = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();

  //
  // Expand the supplied name (may be short name from @context, or full IRI).
  //
  const char* typeIri = corLdExpand(ctxP, typeWild, &corRest.kalloc, NULL, NULL);
  if (typeIri == NULL)
    typeIri = typeWild;

  //
  // Aggregate with details (we need attrTypes / entityCount).
  //
  CorNode* aggregated = NULL;
  int r = db.typeList(tenantP, true, &aggregated);
  if (r != DB_OK || aggregated == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "type discovery failed");
    return true;
  }

  if (!corNgsild.local && tenantP->regCacheP != NULL)
    ldDiscoveryRegAugmentTypes(aggregated, (LdRegCache*) tenantP->regCacheP, true);

  if (!corNgsild.local && !corNgsild.noForward &&
      tenantP->regCacheP != NULL &&
      ldDiscoveryShouldForward())
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
    ldDiscoveryForwardType(aggregated, (LdRegCache*) tenantP->regCacheP,
                           typeIri, typeWild, ownAlias);
  }

  CorNode* entry = NULL;
  for (CorNode* e = aggregated->value.head; e != NULL; e = e->next)
  {
    CorNode* iriP = corTreeLookup(e, "typeIri");
    if (iriP != NULL && iriP->type == CorString && strcmp(iriP->value.s, typeIri) == 0)
    {
      entry = e;
      break;
    }
  }

  if (entry == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "entity type '%s' not found", typeWild);
    return true;
  }

  //
  // EntityTypeInfo (§ 5.2.26):
  //   { id, type:"EntityTypeInfo", typeName, entityCount,
  //     attributeDetails: [ Attribute-restricted ] }
  //
  CorNode* body = corTreeObject(corRest.kallocP, NULL);
  corTreeChildAdd(body, corTreeString(corRest.kallocP, "id", typeIri));
  corTreeChildAdd(body, corTreeString(corRest.kallocP, "type", "EntityTypeInfo"));
  corTreeChildAdd(body, corTreeString(corRest.kallocP, "typeName", shortOrSelf(ctxP, typeIri)));

  CorNode* countP = corTreeLookup(entry, "entityCount");
  corTreeChildAdd(body, corTreeInteger(corRest.kallocP, "entityCount",
                             (countP != NULL) ? countP->value.i : 0));

  CorNode* attrDetails = corTreeArray(corRest.kallocP, "attributeDetails");
  CorNode* attrs      = corTreeLookup(entry, "attrs");
  CorNode* attrTypes  = corTreeLookup(entry, "attrTypes");

  if (attrs != NULL)
  {
    for (CorNode* aN = attrs->value.head; aN != NULL; aN = aN->next)
    {
      if (aN->type != CorString) continue;

      CorNode* ad = corTreeObject(corRest.kallocP, NULL);
      corTreeChildAdd(ad, corTreeString(corRest.kallocP, "id", aN->value.s));
      corTreeChildAdd(ad, corTreeString(corRest.kallocP, "type", "Attribute"));
      corTreeChildAdd(ad, corTreeString(corRest.kallocP, "attributeName", shortOrSelf(ctxP, aN->value.s)));

      CorNode* atArr = corTreeArray(corRest.kallocP, "attributeTypes");
      CorNode* atSrc = (attrTypes != NULL) ? corTreeLookup(attrTypes, aN->value.s) : NULL;
      if (atSrc != NULL && atSrc->type == CorArray)
      {
        for (CorNode* atN = atSrc->value.head; atN != NULL; atN = atN->next)
          if (atN->type == CorString)
            corTreeChildAdd(atArr, corTreeString(corRest.kallocP, NULL, atN->value.s));
      }
      corTreeChildAdd(ad, atArr);

      corTreeChildAdd(attrDetails, ad);
    }
  }
  corTreeChildAdd(body, attrDetails);

  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}
