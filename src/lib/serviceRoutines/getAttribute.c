//
// FILE            getAttribute.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/attributes/{attrId} — Retrieve Available Attribute
// Information (§ 5.7.10). Returns full Attribute (§ 5.2.28): id, type,
// attributeName, attributeCount, attributeTypes, typeNames.
//
// Mode 1 only (local). Modes 2/3 to follow.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                       // corRest
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjObject, kjString, kjInteger, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "corJsonld/corLdCompact.h"                     // corLdCompact
#include "corJsonld/corLdExpand.h"                      // corLdExpand
#include "corJsonld/corLdInit.h"                        // corLdCoreContext

#include "corNgsild/corNgsild.h"                        // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdRegCache.h"                      // LdRegCache
#include "corNgsild/ldDiscovery.h"                     // ldDiscoveryRegAugmentAttrs
#include "corNgsild/ldDiscoveryForward.h"              // ldDiscoveryForwardAttr, ldDiscoveryShouldForward
#include "corNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/getAttribute.h"             // Own interface



static const char* shortOrSelf(CorLdContext* ctxP, const char* iri)
{
  const char* compact = corLdCompact(ctxP, iri);
  return (compact != NULL) ? compact : iri;
}



bool getAttribute(void)
{
  Tenant*     tenantP  = (Tenant*) corNgsild.tenantP;
  const char* attrWild = corRest.in.wildcard[0];

  if (db.attrList == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "attribute discovery not supported by this DB plugin");
    return true;
  }

  CorLdContext* ctxP = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();

  const char* attrIri = corLdExpand(ctxP, attrWild, &corRest.kalloc, NULL, NULL);
  if (attrIri == NULL)
    attrIri = attrWild;

  KjNode* aggregated = NULL;
  int r = db.attrList(tenantP, true, &aggregated);
  if (r != DB_OK || aggregated == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "attribute discovery failed");
    return true;
  }

  if (!corNgsild.local && tenantP->regCacheP != NULL)
    ldDiscoveryRegAugmentAttrs(aggregated, (LdRegCache*) tenantP->regCacheP, true);

  if (!corNgsild.local && !corNgsild.noForward &&
      tenantP->regCacheP != NULL &&
      ldDiscoveryShouldForward())
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
    ldDiscoveryForwardAttr(aggregated, (LdRegCache*) tenantP->regCacheP,
                           attrIri, attrWild, ownAlias);
  }

  KjNode* entry = NULL;
  for (KjNode* e = aggregated->value.firstChildP; e != NULL; e = e->next)
  {
    KjNode* iriP = kjLookup(e, "attrIri");
    if (iriP != NULL && iriP->type == KjString && strcmp(iriP->value.s, attrIri) == 0)
    {
      entry = e;
      break;
    }
  }

  if (entry == NULL)
  {
    ldError(404, LD_ERROR_RESOURCE_NOT_FOUND, "Not Found",
            "attribute '%s' not found", attrWild);
    return true;
  }

  //
  // Attribute (§ 5.2.28): id, type, attributeName, attributeCount,
  //                       attributeTypes, typeNames
  //
  KjNode* body = kjObject(corRest.kjsonP, NULL);
  kjChildAdd(body, kjString(corRest.kjsonP, "id",            attrIri));
  kjChildAdd(body, kjString(corRest.kjsonP, "type",          "Attribute"));
  kjChildAdd(body, kjString(corRest.kjsonP, "attributeName", shortOrSelf(ctxP, attrIri)));

  KjNode* countP = kjLookup(entry, "attrCount");
  kjChildAdd(body, kjInteger(corRest.kjsonP, "attributeCount",
                             (countP != NULL) ? countP->value.i : 0));

  KjNode* at = kjArray(corRest.kjsonP, "attributeTypes");
  KjNode* atSrc = kjLookup(entry, "attrTypes");
  if (atSrc != NULL && atSrc->type == KjArray)
  {
    for (KjNode* t = atSrc->value.firstChildP; t != NULL; t = t->next)
      if (t->type == KjString)
        kjChildAdd(at, kjString(corRest.kjsonP, NULL, t->value.s));
  }
  kjChildAdd(body, at);

  KjNode* tn = kjArray(corRest.kjsonP, "typeNames");
  KjNode* tnSrc = kjLookup(entry, "typeNames");
  if (tnSrc != NULL && tnSrc->type == KjArray)
  {
    for (KjNode* t = tnSrc->value.firstChildP; t != NULL; t = t->next)
      if (t->type == KjString)
        kjChildAdd(tn, kjString(corRest.kjsonP, NULL, shortOrSelf(ctxP, t->value.s)));
  }
  kjChildAdd(body, tn);

  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}
