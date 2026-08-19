//
// FILE            getAttributes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/attributes — Retrieve Available Attributes (§ 5.7.8) /
// Retrieve Details of Available Attributes (§ 5.7.9) when ?details=true.
//
// Mode 1 only (local). Modes 2/3 to follow.
//

#include <stddef.h>                                   // NULL

#include "corRest/CorRestState.h"                       // corRest
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjObject, kjString, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "corJsonld/corLdCompact.h"                     // corLdCompact
#include "corJsonld/corLdInit.h"                        // corLdCoreContext

#include "corNgsild/corNgsild.h"                        // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdRegCache.h"                      // LdRegCache
#include "corNgsild/ldDiscovery.h"                     // ldDiscoveryRegAugmentAttrs
#include "corNgsild/ldDiscoveryForward.h"              // ldDiscoveryForwardAttrs, ldDiscoveryShouldForward
#include "corNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/getAttributes.h"            // Own interface



static const char* shortOrSelf(CorLdContext* ctxP, const char* iri)
{
  const char* compact = corLdCompact(ctxP, iri);
  return (compact != NULL) ? compact : iri;
}



bool getAttributes(void)
{
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  if (db.attrList == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "attribute discovery not supported by this DB plugin");
    return true;
  }

  bool details = corNgsild.details;

  KjNode* aggregated = NULL;
  int r = db.attrList(tenantP, details, &aggregated);
  if (r != DB_OK || aggregated == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "attribute discovery failed");
    return true;
  }

  if (!corNgsild.local && tenantP->regCacheP != NULL)
    ldDiscoveryRegAugmentAttrs(aggregated, (LdRegCache*) tenantP->regCacheP, details);

  if (!corNgsild.local && !corNgsild.noForward &&
      tenantP->regCacheP != NULL &&
      ldDiscoveryShouldForward())
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
    ldDiscoveryForwardAttrs(aggregated, (LdRegCache*) tenantP->regCacheP, details, ownAlias);
  }

  CorLdContext* ctxP = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();

  if (!details)
  {
    //
    // AttributeList (§ 5.2.27): { id, type:"AttributeList", attributeList:[short names] }
    //
    KjNode* body = kjObject(corRest.kjsonP, NULL);
    kjChildAdd(body, kjString(corRest.kjsonP, "id",   "urn:ngsi-ld:AttributeList:local"));
    kjChildAdd(body, kjString(corRest.kjsonP, "type", "AttributeList"));

    KjNode* attrList = kjArray(corRest.kjsonP, "attributeList");
    for (KjNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
    {
      KjNode* iriP = kjLookup(entry, "attrIri");
      if (iriP == NULL || iriP->type != KjString) continue;
      kjChildAdd(attrList, kjString(corRest.kjsonP, NULL, shortOrSelf(ctxP, iriP->value.s)));
    }
    kjChildAdd(body, attrList);

    corRest.out.responseTree   = body;
    corRest.out.httpStatusCode = 200;
    return true;
  }

  //
  // Details: Attribute[] (§ 5.2.28) restricted to id, type, attributeName, typeNames
  //
  KjNode* body = kjArray(corRest.kjsonP, NULL);

  for (KjNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
  {
    KjNode* iriP = kjLookup(entry, "attrIri");
    if (iriP == NULL || iriP->type != KjString) continue;

    KjNode* obj = kjObject(corRest.kjsonP, NULL);
    kjChildAdd(obj, kjString(corRest.kjsonP, "id",            iriP->value.s));
    kjChildAdd(obj, kjString(corRest.kjsonP, "type",          "Attribute"));
    kjChildAdd(obj, kjString(corRest.kjsonP, "attributeName", shortOrSelf(ctxP, iriP->value.s)));

    KjNode* tn = kjArray(corRest.kjsonP, "typeNames");
    KjNode* tnSrc = kjLookup(entry, "typeNames");
    if (tnSrc != NULL && tnSrc->type == KjArray)
    {
      for (KjNode* t = tnSrc->value.firstChildP; t != NULL; t = t->next)
        if (t->type == KjString)
          kjChildAdd(tn, kjString(corRest.kjsonP, NULL, shortOrSelf(ctxP, t->value.s)));
    }
    kjChildAdd(obj, tn);

    kjChildAdd(body, obj);
  }

  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}
