//
// FILE            getAttributes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
//
// GET /ngsi-ld/v1/attributes — Retrieve Available Attributes (§ 5.7.8) /
// Retrieve Details of Available Attributes (§ 5.7.9) when ?details=true.
//
// Mode 1 only (local). Modes 2/3 to follow.
//

#include <stddef.h>                                   // NULL

#include "swRest/SwRestState.h"                       // swRest
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjObject, kjString, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup

#include "swJsonld/swldCompact.h"                     // swldCompact
#include "swJsonld/swldInit.h"                        // swldCoreContext

#include "swNgsild/swNgsild.h"                        // ldError, LD_ERROR_*, swNgsild
#include "swNgsild/LdRegCache.h"                      // LdRegCache
#include "swNgsild/ldDiscovery.h"                     // ldDiscoveryRegAugmentAttrs
#include "swNgsild/ldDiscoveryForward.h"              // ldDiscoveryForwardAttrs, ldDiscoveryShouldForward
#include "swNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/getAttributes.h"            // Own interface



static const char* shortOrSelf(SwldContext* ctxP, const char* iri)
{
  const char* compact = swldCompact(ctxP, iri);
  return (compact != NULL) ? compact : iri;
}



bool getAttributes(void)
{
  Tenant* tenantP = (Tenant*) swNgsild.tenantP;

  if (db.attrList == NULL)
  {
    ldError(501, LD_ERROR_INTERNAL_ERROR, "Not Implemented",
            "attribute discovery not supported by this DB plugin");
    return true;
  }

  bool details = swNgsild.details;

  KjNode* aggregated = NULL;
  int r = db.attrList(tenantP, details, &aggregated);
  if (r != DB_OK || aggregated == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "attribute discovery failed");
    return true;
  }

  if (!swNgsild.local && tenantP->regCacheP != NULL)
    ldDiscoveryRegAugmentAttrs(aggregated, (LdRegCache*) tenantP->regCacheP, details);

  if (!swNgsild.local && !swNgsild.noForward &&
      tenantP->regCacheP != NULL &&
      ldDiscoveryShouldForward())
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &swRest.kalloc);
    ldDiscoveryForwardAttrs(aggregated, (LdRegCache*) tenantP->regCacheP, details, ownAlias);
  }

  SwldContext* ctxP = (swNgsild.contextP != NULL) ? swNgsild.contextP : swldCoreContext();

  if (!details)
  {
    //
    // AttributeList (§ 5.2.27): { id, type:"AttributeList", attributeList:[short names] }
    //
    KjNode* body = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(body, kjString(swRest.kjsonP, "id",   "urn:ngsi-ld:AttributeList:local"));
    kjChildAdd(body, kjString(swRest.kjsonP, "type", "AttributeList"));

    KjNode* attrList = kjArray(swRest.kjsonP, "attributeList");
    for (KjNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
    {
      KjNode* iriP = kjLookup(entry, "attrIri");
      if (iriP == NULL || iriP->type != KjString) continue;
      kjChildAdd(attrList, kjString(swRest.kjsonP, NULL, shortOrSelf(ctxP, iriP->value.s)));
    }
    kjChildAdd(body, attrList);

    swRest.out.responseTree   = body;
    swRest.out.httpStatusCode = 200;
    return true;
  }

  //
  // Details: Attribute[] (§ 5.2.28) restricted to id, type, attributeName, typeNames
  //
  KjNode* body = kjArray(swRest.kjsonP, NULL);

  for (KjNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
  {
    KjNode* iriP = kjLookup(entry, "attrIri");
    if (iriP == NULL || iriP->type != KjString) continue;

    KjNode* obj = kjObject(swRest.kjsonP, NULL);
    kjChildAdd(obj, kjString(swRest.kjsonP, "id",            iriP->value.s));
    kjChildAdd(obj, kjString(swRest.kjsonP, "type",          "Attribute"));
    kjChildAdd(obj, kjString(swRest.kjsonP, "attributeName", shortOrSelf(ctxP, iriP->value.s)));

    KjNode* tn = kjArray(swRest.kjsonP, "typeNames");
    KjNode* tnSrc = kjLookup(entry, "typeNames");
    if (tnSrc != NULL && tnSrc->type == KjArray)
    {
      for (KjNode* t = tnSrc->value.firstChildP; t != NULL; t = t->next)
        if (t->type == KjString)
          kjChildAdd(tn, kjString(swRest.kjsonP, NULL, shortOrSelf(ctxP, t->value.s)));
    }
    kjChildAdd(obj, tn);

    kjChildAdd(body, obj);
  }

  swRest.out.responseTree   = body;
  swRest.out.httpStatusCode = 200;
  return true;
}
