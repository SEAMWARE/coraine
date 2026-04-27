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

#include "swRest/SwRestState.h"                       // swRest
#include "kalloc/kaAlloc.h"                           // kaAlloc
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "swJsonld/swldCompact.h"                     // swldCompact
#include "swJsonld/swldExpand.h"                      // swldExpand
#include "swJsonld/swldInit.h"                        // swldCoreContext

#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdRegCache.h"                      // LdRegCache
#include "swNgsild/ldDiscovery.h"                     // ldDiscoveryRegAugmentTypes
#include "swNgsild/ldDiscoveryForward.h"              // ldDiscoveryForwardType, ldDiscoveryShouldForward
#include "swNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/getType.h"                  // Own interface



// -----------------------------------------------------------------------------
//
// shortOrSelf -
//
static const char* shortOrSelf(SwldContext* ctxP, const char* iri)
{
  const char* compact = swldCompact(ctxP, iri);
  return (compact != NULL) ? compact : iri;
}



// -----------------------------------------------------------------------------
//
// getType -
//
bool getType(void)
{
  Tenant*     tenantP    = (Tenant*) swNgsild.tenantP;
  const char* typeWild   = swRest.in.wildcard[0];    // url-decoded already

  if (db.typeList == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
            "type discovery not supported by this DB plugin");
    return true;
  }

  SwldContext* ctxP = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();

  //
  // Expand the supplied name (may be short name from @context, or full IRI).
  //
  const char* typeIri = swldExpand(ctxP, typeWild, &swRest.kalloc, NULL, NULL);
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

  if (!swNgsild.local && tenantP->regCacheP != NULL)
    ldDiscoveryRegAugmentTypes(aggregated, (LdRegCache*) tenantP->regCacheP, true);

  if (!swNgsild.local && !swNgsild.noForward &&
      tenantP->regCacheP != NULL &&
      ldDiscoveryShouldForward())
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);
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
  KjNode* body = kjObject(swRest.kjsonP, NULL);
  kjChildAdd(body, kjString(swRest.kjsonP, "id",       typeIri));
  kjChildAdd(body, kjString(swRest.kjsonP, "type",     "EntityTypeInfo"));
  kjChildAdd(body, kjString(swRest.kjsonP, "typeName", shortOrSelf(ctxP, typeIri)));

  KjNode* countP = kjLookup(entry, "entityCount");
  kjChildAdd(body, kjInteger(swRest.kjsonP, "entityCount",
                             (countP != NULL) ? countP->value.i : 0));

  KjNode* attrDetails = kjArray(swRest.kjsonP, "attributeDetails");
  KjNode* attrs       = kjLookup(entry, "attrs");
  KjNode* attrTypes   = kjLookup(entry, "attrTypes");

  if (attrs != NULL)
  {
    for (KjNode* aN = attrs->value.firstChildP; aN != NULL; aN = aN->next)
    {
      if (aN->type != KjString) continue;

      KjNode* ad = kjObject(swRest.kjsonP, NULL);
      kjChildAdd(ad, kjString(swRest.kjsonP, "id",            aN->value.s));
      kjChildAdd(ad, kjString(swRest.kjsonP, "type",          "Attribute"));
      kjChildAdd(ad, kjString(swRest.kjsonP, "attributeName", shortOrSelf(ctxP, aN->value.s)));

      KjNode* atArr = kjArray(swRest.kjsonP, "attributeTypes");
      KjNode* atSrc = (attrTypes != NULL) ? kjLookup(attrTypes, aN->value.s) : NULL;
      if (atSrc != NULL && atSrc->type == KjArray)
      {
        for (KjNode* atN = atSrc->value.firstChildP; atN != NULL; atN = atN->next)
          if (atN->type == KjString)
            kjChildAdd(atArr, kjString(swRest.kjsonP, NULL, atN->value.s));
      }
      kjChildAdd(ad, atArr);

      kjChildAdd(attrDetails, ad);
    }
  }
  kjChildAdd(body, attrDetails);

  swRest.out.responseTree   = body;
  swRest.out.httpStatusCode = 200;
  return true;
}
