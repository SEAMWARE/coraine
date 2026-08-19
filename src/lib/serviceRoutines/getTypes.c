//
// FILE            getTypes.c
//
// AUTHOR          Ken Zangelin
//
// Copyright 2026 Seamware
// SPDX-License-Identifier: Apache-2.0
//
// GET /ngsi-ld/v1/types — Retrieve Available Entity Types (§ 5.7.5) /
// Retrieve Details of Available Entity Types (§ 5.7.6) when ?details=true.
//
// Mode 1 (?local=true): only local entities are considered. Modes 2/3
// (CSR metadata, dispatch) are not yet wired — the default request is
// currently treated as mode 1.
//

#include <stddef.h>                                   // NULL
#include <string.h>                                   // strcmp

#include "corRest/CorRestState.h"                       // corRest
#include "kjson/KjNode.h"                             // KjNode
#include "kjson/kjBuilder.h"                          // kjArray, kjObject, kjString, kjChildAdd
#include "kjson/kjLookup.h"                           // kjLookup
#include "kjson/kjClone.h"                            // kjClone

#include "corJsonld/corLdCompact.h"                     // corLdCompact
#include "corJsonld/corLdInit.h"                        // corLdCoreContext

#include "corNgsild/corNgsild.h"                        // ldError, LD_ERROR_*, corNgsild
#include "corNgsild/LdVocab.h"                         // LD_VOCAB_*
#include "corNgsild/LdRegCache.h"                      // LdRegCache
#include "corNgsild/ldDiscovery.h"                     // ldDiscoveryRegAugmentTypes
#include "corNgsild/ldDiscoveryForward.h"              // ldDiscoveryForwardTypes, ldDiscoveryShouldForward
#include "corNgsild/ldCsourceAlias.h"                  // ldCsourceAliasForTenant

#include "db/DbDriver.h"                              // db, DB_OK
#include "db/Tenant.h"                                // Tenant

#include "serviceRoutines/getTypes.h"                 // Own interface



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
// getTypes -
//
bool getTypes(void)
{
  Tenant* tenantP = (Tenant*) corNgsild.tenantP;

  if (db.typeList == NULL)
  {
    ldError(422, LD_ERROR_OP_NOT_SUPPORTED, "Not Implemented",
            "type discovery not supported by this DB plugin");
    return true;
  }

  bool details = corNgsild.details;

  KjNode* aggregated = NULL;
  int r = db.typeList(tenantP, details, &aggregated);
  if (r != DB_OK || aggregated == NULL)
  {
    ldError(500, LD_ERROR_INTERNAL_ERROR, "Internal Error",
            "type discovery failed");
    return true;
  }

  //
  // Mode 2 (?noForward=true) or default (mode 3): augment with
  // CSR-declared types/attrs. Mode 1 (?local=true) stops at local data.
  //
  if (!corNgsild.local && tenantP->regCacheP != NULL)
    ldDiscoveryRegAugmentTypes(aggregated, (LdRegCache*) tenantP->regCacheP, details);

  //
  // Mode 3 only (default — !local && !noForward): forward the query to
  // every CSR supporting retrieveEntityType(s) and merge the results.
  //
  if (!corNgsild.local && !corNgsild.noForward &&
      tenantP->regCacheP != NULL &&
      ldDiscoveryShouldForward())
  {
    const char* ownAlias = ldCsourceAliasForTenant(tenantP->name, &corRest.kalloc);
    ldDiscoveryForwardTypes(aggregated, (LdRegCache*) tenantP->regCacheP, details, ownAlias);
  }

  CorLdContext* ctxP = (corNgsild.contextP != NULL) ? corNgsild.contextP : corLdCoreContext();

  if (!details)
  {
    //
    // EntityTypeList (§ 5.2.24): { id, type:"EntityTypeList", typeList:[short names] }
    //
    KjNode* body = kjObject(corRest.kjsonP, NULL);
    kjChildAdd(body, kjString(corRest.kjsonP, "id",   "urn:ngsi-ld:EntityTypeList:local"));
    kjChildAdd(body, kjString(corRest.kjsonP, "type", "EntityTypeList"));

    KjNode* typeList = kjArray(corRest.kjsonP, "typeList");
    for (KjNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
    {
      KjNode* iriP = kjLookup(entry, "typeIri");
      if (iriP == NULL || iriP->type != KjString) continue;
      kjChildAdd(typeList, kjString(corRest.kjsonP, NULL, shortOrSelf(ctxP, iriP->value.s)));
    }
    kjChildAdd(body, typeList);

    corRest.out.responseTree   = body;
    corRest.out.httpStatusCode = 200;
    return true;
  }

  //
  // Details: EntityType[] (§ 5.2.25) — array of
  //   { id: <IRI>, type:"EntityType", typeName:<short>, attributeNames:[<short>] }
  //
  KjNode* body = kjArray(corRest.kjsonP, NULL);

  for (KjNode* entry = aggregated->value.firstChildP; entry != NULL; entry = entry->next)
  {
    KjNode* iriP = kjLookup(entry, "typeIri");
    if (iriP == NULL || iriP->type != KjString) continue;

    KjNode* et = kjObject(corRest.kjsonP, NULL);
    kjChildAdd(et, kjString(corRest.kjsonP, "id",       iriP->value.s));
    kjChildAdd(et, kjString(corRest.kjsonP, "type",     "EntityType"));
    kjChildAdd(et, kjString(corRest.kjsonP, "typeName", shortOrSelf(ctxP, iriP->value.s)));

    KjNode* attrNames = kjArray(corRest.kjsonP, "attributeNames");
    KjNode* attrsAgg  = kjLookup(entry, "attrs");
    if (attrsAgg != NULL)
    {
      for (KjNode* aN = attrsAgg->value.firstChildP; aN != NULL; aN = aN->next)
        if (aN->type == KjString)
          kjChildAdd(attrNames, kjString(corRest.kjsonP, NULL, shortOrSelf(ctxP, aN->value.s)));
    }
    kjChildAdd(et, attrNames);

    kjChildAdd(body, et);
  }

  corRest.out.responseTree   = body;
  corRest.out.httpStatusCode = 200;
  return true;
}
