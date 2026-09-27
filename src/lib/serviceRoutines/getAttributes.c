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
#include "corTree/CorNode.h"                          // CorNode
#include "corTree/corTreeBuilder.h"                   // corTreeArray, corTreeObject, corTreeString, corTreeChildAdd
#include "corTree/corTreeLookup.h"                    // corTreeLookup

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

  CorNode* aggregated = NULL;
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
    CorNode* body = corTreeObject(corRest.kallocP, NULL);
    corTreeChildAdd(body, corTreeString(corRest.kallocP, "id", "urn:ngsi-ld:AttributeList:local"));
    corTreeChildAdd(body, corTreeString(corRest.kallocP, "type", "AttributeList"));

    CorNode* attrList = corTreeArray(corRest.kallocP, "attributeList");
    for (CorNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
    {
      CorNode* iriP = corTreeLookup(entry, "attrIri");
      if (iriP == NULL || iriP->type != CorString) continue;
      corTreeChildAdd(attrList, corTreeString(corRest.kallocP, NULL, shortOrSelf(ctxP, iriP->value.s)));
    }
    corTreeChildAdd(body, attrList);

    corRest.out.responseTree   = body;
    corRest.out.httpStatusCode = 200;
    return true;
  }

  //
  // Details: Attribute[] (§ 5.2.28) restricted to id, type, attributeName, typeNames
  //
  CorNode* body = corTreeArray(corRest.kallocP, NULL);

  for (CorNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
  {
    CorNode* iriP = corTreeLookup(entry, "attrIri");
    if (iriP == NULL || iriP->type != CorString) continue;

    CorNode* obj = corTreeObject(corRest.kallocP, NULL);
    corTreeChildAdd(obj, corTreeString(corRest.kallocP, "id", iriP->value.s));
    corTreeChildAdd(obj, corTreeString(corRest.kallocP, "type", "Attribute"));
    corTreeChildAdd(obj, corTreeString(corRest.kallocP, "attributeName", shortOrSelf(ctxP, iriP->value.s)));

    CorNode* tn = corTreeArray(corRest.kallocP, "typeNames");
    CorNode* tnSrc = corTreeLookup(entry, "typeNames");
    if (tnSrc != NULL && tnSrc->type == CorArray)
    {
      for (CorNode* t = tnSrc->value.firstChildP; t != NULL; t = t->next)
        if (t->type == CorString)
          corTreeChildAdd(tn, corTreeString(corRest.kallocP, NULL, shortOrSelf(ctxP, t->value.s)));
    }
    corTreeChildAdd(obj, tn);

    corTreeChildAdd(body, obj);
  }

  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}
