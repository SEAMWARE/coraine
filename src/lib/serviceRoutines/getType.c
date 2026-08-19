//
// FILE            getType.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
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
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

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
  KjNode* aggregated = NULL;
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

  KjNode* entry = NULL;
  for (KjNode* e = aggregated->value.firstChildP; e != NULL; e = e->next)
  {
    KjNode* iriP = kjLookup(e, "typeIri");
    if (iriP != NULL && iriP->type == KjString && strcmp(iriP->value.s, typeIri) == 0)
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
  KjNode* body = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(body, kjString(corRest.kjsonP, "id",       typeIri));
  kjChildAdd(body, kjString(corRest.kjsonP, "type",     "EntityTypeInfo"));
  kjChildAdd(body, kjString(corRest.kjsonP, "typeName", shortOrSelf(ctxP, typeIri)));

  KjNode* countP = kjLookup(entry, "entityCount");
  kjChildAdd(body, kjInteger(corRest.kjsonP, "entityCount",
                             (countP != NULL) ? countP->value.i : 0));

  KjNode* attrDetails = kjArray(corRest.kjsonP, "attributeDetails");
  KjNode* attrs       = kjLookup(entry, "attrs");
  KjNode* attrTypes   = kjLookup(entry, "attrTypes");

  if (attrs != NULL)
  {
    for (KjNode* aN = attrs->value.firstChildP; aN != NULL; aN = aN->next)
    {
      if (aN->type != KjString) continue;

      KjNode* ad = kjObject(corRest.kjsonP, NULL);
      kjChildAdd(ad, kjString(corRest.kjsonP, "id",            aN->value.s));
      kjChildAdd(ad, kjString(corRest.kjsonP, "type",          "Attribute"));
      kjChildAdd(ad, kjString(corRest.kjsonP, "attributeName", shortOrSelf(ctxP, aN->value.s)));

      KjNode* atArr = kjArray(corRest.kjsonP, "attributeTypes");
      KjNode* atSrc = (attrTypes != NULL) ? kjLookup(attrTypes, aN->value.s) : NULL;
      if (atSrc != NULL && atSrc->type == KjArray)
      {
        for (KjNode* atN = atSrc->value.firstChildP; atN != NULL; atN = atN->next)
          if (atN->type == KjString)
            kjChildAdd(atArr, kjString(corRest.kjsonP, NULL, atN->value.s));
      }
      kjChildAdd(ad, atArr);

      kjChildAdd(attrDetails, ad);
    }
  }
  kjChildAdd(body, attrDetails);

  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}
