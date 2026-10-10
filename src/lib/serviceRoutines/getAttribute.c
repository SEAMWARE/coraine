//
// FILE            getAttribute.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
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
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeObject, corTreeString, corTreeInteger, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup

#include "corJsonld/corLdCompact.h"                   // corLdCompact
#include "corJsonld/corLdExpand.h"                      // corLdExpand
#include "corJsonld/corLdInit.h"                        // corLdCoreContext

#include "corNgsild/corNgsild.h"                        // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdRegCache.h"                      // LdRegCache
#include "corNgsild/ldDiscovery.h"                     // ldDiscoveryRegAugmentAttrs
#include "corNgsild/ldDiscoveryForward.h"              // ldDiscoveryForwardAttr, ldDiscoveryShouldForward
#include "corNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/discoveryIri.h"             // discoveryIri
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

  CorNode* aggregated = NULL;
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

  CorNode* entry = NULL;
  for (CorNode* e = aggregated->value.head; e != NULL; e = e->next)
  {
    CorNode* iriP = corTreeLookup(e, "attrIri");
    if (iriP != NULL && iriP->type == CorString && strcmp(iriP->value.s, attrIri) == 0)
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
  CorNode* body = corTreeObject(corRest.kallocP, NULL);
  corTreeChildAdd(body, corTreeString(corRest.kallocP, "id", discoveryIri(attrIri)));
  corTreeChildAdd(body, corTreeString(corRest.kallocP, "type", "Attribute"));
  corTreeChildAdd(body, corTreeString(corRest.kallocP, "attributeName", shortOrSelf(ctxP, attrIri)));

  CorNode* countP = corTreeLookup(entry, "attrCount");
  corTreeChildAdd(body, corTreeInteger(corRest.kallocP, "attributeCount",
                             (countP != NULL) ? countP->value.i : 0));

  CorNode* at = corTreeArray(corRest.kallocP, "attributeTypes");
  CorNode* atSrc = corTreeLookup(entry, "attrTypes");
  if (atSrc != NULL && atSrc->type == CorArray)
  {
    for (CorNode* t = atSrc->value.head; t != NULL; t = t->next)
      if (t->type == CorString)
        corTreeChildAdd(at, corTreeString(corRest.kallocP, NULL, t->value.s));
  }
  corTreeChildAdd(body, at);

  CorNode* tn = corTreeArray(corRest.kallocP, "typeNames");
  CorNode* tnSrc = corTreeLookup(entry, "typeNames");
  if (tnSrc != NULL && tnSrc->type == CorArray)
  {
    for (CorNode* t = tnSrc->value.head; t != NULL; t = t->next)
      if (t->type == CorString)
        corTreeChildAdd(tn, corTreeString(corRest.kallocP, NULL, shortOrSelf(ctxP, t->value.s)));
  }
  corTreeChildAdd(body, tn);

  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}
